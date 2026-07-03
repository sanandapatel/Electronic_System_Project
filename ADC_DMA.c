/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body - Optimized FFT Engine (Legacy IOC Compatible)
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "string.h"
#include "arm_math.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
#define FFT_SIZE 1024
#define SYNC_BYTE_1 0xAA
#define SYNC_BYTE_2 0x55
#define FFT_HEADER  0xA55AA55A

// Natural alignment for Cortex-M7 (no packed attribute needed)
typedef struct {
    uint8_t sync[2];                  // Sync marker
    uint32_t header;                  // 0xA55AA55A
    uint32_t frame_id;                // Monotonically increasing
    uint16_t magnitude[FFT_SIZE/2];   // Q15 fixed-point magnitudes
    uint8_t checksum;                 // XOR checksum
} FFTPacket_t;
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define ADC_BUFFER_SIZE        1024
#define TX_QUEUE_DEPTH          8
#define TX_QUEUE_INDEX_MASK    (TX_QUEUE_DEPTH - 1)
#define PACKET_SIZE_BYTES       sizeof(FFTPacket_t)
#define PACKET_SIZE_U16         ((PACKET_SIZE_BYTES + 1) / sizeof(uint16_t))
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */
/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
#if defined ( __ICCARM__ )
#pragma location=0x2007c000
ETH_DMADescTypeDef  DMARxDscrTab[ETH_RX_DESC_CNT];
#pragma location=0x2007c0a0
ETH_DMADescTypeDef  DMATxDscrTab[ETH_TX_DESC_CNT];
#elif defined ( __CC_ARM )
__attribute__((at(0x2007c000))) ETH_DMADescTypeDef  DMARxDscrTab[ETH_RX_DESC_CNT];
__attribute__((at(0x2007c0a0))) ETH_DMADescTypeDef  DMATxDscrTab[ETH_TX_DESC_CNT];
#elif defined ( __GNUC__ )
ETH_DMADescTypeDef DMARxDscrTab[ETH_RX_DESC_CNT] __attribute__((section(".RxDecripSection")));
ETH_DMADescTypeDef DMATxDscrTab[ETH_TX_DESC_CNT] __attribute__((section(".TxDecripSection")));
#endif

ETH_TxPacketConfig TxConfig;
ADC_HandleTypeDef hadc1;
DMA_HandleTypeDef hdma_adc1;
ETH_HandleTypeDef heth;
TIM_HandleTypeDef htim2;
UART_HandleTypeDef huart2;      // Preserved from legacy IOC
UART_HandleTypeDef huart3;
DMA_HandleTypeDef hdma_usart3_tx;
PCD_HandleTypeDef hpcd_USB_OTG_FS;

/* USER CODE BEGIN PV */
uint16_t adc_buffer[ADC_BUFFER_SIZE] __attribute__((aligned(32)));
static uint16_t tx_queue[TX_QUEUE_DEPTH][PACKET_SIZE_U16] __attribute__((aligned(32)));

static volatile uint16_t queue_head  = 0;
static volatile uint16_t queue_tail  = 0;
static volatile uint16_t queue_count = 0;
static volatile uint8_t uart_tx_busy = 0;

// DSP Engine State (32-byte aligned for cache safety)
static arm_rfft_fast_instance_f32 S;
static float32_t fft_input[FFT_SIZE] __attribute__((aligned(32)));
static float32_t fft_output[FFT_SIZE] __attribute__((aligned(32)));
static float32_t fft_magnitude[FFT_SIZE / 2] __attribute__((aligned(32)));
static float32_t hann_window[FFT_SIZE] __attribute__((aligned(32)));
static volatile uint32_t frame_id = 0;

// Processing flag - set by ISR, cleared by main loop
static volatile uint8_t fft_ready = 0;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MPU_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_ETH_Init(void);
static void MX_USB_OTG_FS_PCD_Init(void);
static void MX_ADC1_Init(void);
static void MX_TIM2_Init(void);
static void MX_USART2_UART_Init(void);  // Preserved from legacy IOC
static void MX_USART3_UART_Init(void);

/* USER CODE BEGIN PFP */
static void UART_Queue_Service(void);
static void FFT_Queue_Enqueue(const void *src);
void DSP_Init(void);
void DSP_ProcessFrame(void);
/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

void DSP_Init(void)
{
    arm_rfft_fast_init_f32(&S, FFT_SIZE);
    // Precompute Hann window ONCE at startup
    for (uint16_t i = 0; i < FFT_SIZE; i++) {
        hann_window[i] = 0.5f * (1.0f - cosf(2.0f * PI * i / (FFT_SIZE - 1)));
    }
}

static void FFT_Queue_Enqueue(const void *src)
{
    if (queue_count >= TX_QUEUE_DEPTH) return; // Graceful drop
    memcpy(&tx_queue[queue_tail][0], src, PACKET_SIZE_BYTES);
    queue_tail = (queue_tail + 1) & TX_QUEUE_INDEX_MASK;
    queue_count++;
}

void DSP_ProcessFrame(void)
{
    float32_t mean;

    // DC removal using CMSIS SIMD
    arm_mean_f32(fft_input, FFT_SIZE, &mean);
    arm_offset_f32(fft_input, -mean, fft_input, FFT_SIZE);

    // Apply precomputed Hann window in-place
    arm_mult_f32(fft_input, hann_window, fft_input, FFT_SIZE);

    // Radix-4/2 mixed FFT optimized for Cortex-M7
    arm_rfft_fast_f32(&S, fft_input, fft_output, 0);

    // Complex magnitude via SIMD
    arm_cmplx_mag_f32(fft_output, fft_magnitude, FFT_SIZE / 2);

    // CORRECT SCALING: 4/N compensates for FFT length AND Hann window coherent gain (0.5)
    arm_scale_f32(fft_magnitude, 4.0f / FFT_SIZE, fft_magnitude, FFT_SIZE / 2);

    // Pack into binary packet with Q15 fixed-point
    static FFTPacket_t pkt;
    pkt.sync[0] = SYNC_BYTE_1;
    pkt.sync[1] = SYNC_BYTE_2;
    pkt.header = FFT_HEADER;
    pkt.frame_id = frame_id++;

    // Convert float magnitude to Q15 (0.0→0, 1.0→32767)
    const float32_t scale = 32767.0f;
    for(uint16_t i = 0; i < FFT_SIZE/2; i++) {
        int32_t val = (int32_t)(fft_magnitude[i] * scale + 0.5f);
        pkt.magnitude[i] = (val > 32767) ? 32767 : (val < 0) ? 0 : (uint16_t)val;
    }

    // XOR checksum over all preceding bytes
    pkt.checksum = 0;
    uint8_t *bytes = (uint8_t*)&pkt;
    for(size_t i = 0; i < sizeof(pkt)-1; i++)
        pkt.checksum ^= bytes[i];

    FFT_Queue_Enqueue(&pkt);
}

// Half-transfer: ONLY for LED heartbeat visualization
void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1) {
        HAL_GPIO_TogglePin(GPIOB, LD3_Pin);
    }
}

// Full-transfer: Cache invalidate + flag set ONLY. NO DSP.
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc)
{
    if (hadc->Instance == ADC1) {
        // Invalidate second half of buffer
        SCB_InvalidateDCache_by_Addr(
            (uint32_t *)&adc_buffer[ADC_BUFFER_SIZE / 2],
            (ADC_BUFFER_SIZE / 2) * sizeof(uint16_t)
        );
        fft_ready = 1;
    }
}

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3) {
        queue_head = (queue_head + 1) & TX_QUEUE_INDEX_MASK;
        queue_count--;
        uart_tx_busy = 0;
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart->Instance == USART3) {
        if (uart_tx_busy) {
            queue_head = (queue_head + 1) & TX_QUEUE_INDEX_MASK;
            if (queue_count > 0) queue_count--;
            uart_tx_busy = 0;
        }
        __HAL_UART_CLEAR_PEFLAG(huart);
    }
}

static void UART_Queue_Service(void)
{
    if ((uart_tx_busy == 0) && (queue_count > 0)) {
        uart_tx_busy = 1;
        // Clean cache BEFORE DMA reads CPU-written data
        SCB_CleanDCache_by_Addr(
            (uint32_t *)&tx_queue[queue_head][0],
            PACKET_SIZE_BYTES
        );

        if (HAL_UART_Transmit_DMA(&huart3,
                                   (uint8_t *)&tx_queue[queue_head][0],
                                   PACKET_SIZE_BYTES) != HAL_OK) {
            uart_tx_busy = 0;
        }
    }
}
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{
  /* USER CODE BEGIN 1 */
  /* USER CODE END 1 */

  /* USER CODE BEGIN SysInit */
  // SAFE ORDER: MPU → Caches → HAL → Clock
  MPU_Config();
  SCB_EnableICache();
  SCB_EnableDCache();
  HAL_Init();
  SystemClock_Config();
  /* USER CODE END SysInit */

  // Legacy IOC peripherals preserved exactly as generated
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_ETH_Init();
  MX_USB_OTG_FS_PCD_Init();
  MX_ADC1_Init();
  MX_TIM2_Init();
  MX_USART2_UART_Init();  // Unused but preserved
  MX_USART3_UART_Init();

  /* USER CODE BEGIN 2 */
  DSP_Init();

  const char *test_msg = "USART3 FFT Stream Started!\r\n";
  HAL_UART_Transmit(&huart3, (uint8_t *)test_msg, strlen(test_msg), 1000);

  // Start peripherals (order preserved from legacy IOC)
  if (HAL_TIM_Base_Start(&htim2) != HAL_OK)
    Error_Handler();

  if (HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc_buffer, ADC_BUFFER_SIZE) != HAL_OK)
    Error_Handler();
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
      UART_Queue_Service();

      // Process DSP in main loop when flag is set
      if (fft_ready) {
          fft_ready = 0;

          // CORRECT: Unsigned uint16 to float32 conversion
          for(uint32_t i = 0; i < FFT_SIZE; i++) {
              fft_input[i] = (float32_t)adc_buffer[i];
          }

          DSP_ProcessFrame();
      }
      /* USER CODE END WHILE */

      /* USER CODE BEGIN 3 */
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  HAL_PWR_EnableBkUpAccess();
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE3);

  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_BYPASS;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 96;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 4;
  RCC_OscInitStruct.PLL.PLLR = 2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) Error_Handler();

  if (HAL_PWREx_EnableOverDrive() != HAL_OK) Error_Handler();

  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_3) != HAL_OK) Error_Handler();
}

/**
  * @brief ADC1 Initialization Function (Preserved from legacy IOC)
  */
static void MX_ADC1_Init(void)
{
  ADC_ChannelConfTypeDef sConfig = {0};
  ADC_InjectionConfTypeDef sConfigInjected = {0};

  hadc1.Instance = ADC1;
  hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
  hadc1.Init.Resolution = ADC_RESOLUTION_12B;
  hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
  hadc1.Init.ContinuousConvMode = DISABLE;
  hadc1.Init.DiscontinuousConvMode = DISABLE;
  hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_RISING;
  hadc1.Init.ExternalTrigConv = ADC_EXTERNALTRIGCONV_T2_TRGO;
  hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
  hadc1.Init.NbrOfConversion = 1;
  hadc1.Init.DMAContinuousRequests = ENABLE;
  hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
  if (HAL_ADC_Init(&hadc1) != HAL_OK) Error_Handler();

  sConfig.Channel = ADC_CHANNEL_0;
  sConfig.Rank = ADC_REGULAR_RANK_1;
  sConfig.SamplingTime = ADC_SAMPLETIME_56CYCLES;
  if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) Error_Handler();

  // Injected channel configuration PRESERVED from legacy IOC
  sConfigInjected.InjectedChannel = ADC_CHANNEL_0;
  sConfigInjected.InjectedRank = ADC_INJECTED_RANK_1;
  sConfigInjected.InjectedNbrOfConversion = 1;
  sConfigInjected.InjectedSamplingTime = ADC_SAMPLETIME_56CYCLES;
  sConfigInjected.ExternalTrigInjecConvEdge = ADC_EXTERNALTRIGINJECCONVEDGE_NONE;
  sConfigInjected.ExternalTrigInjecConv = ADC_INJECTED_SOFTWARE_START;
  sConfigInjected.AutoInjectedConv = DISABLE;
  sConfigInjected.InjectedDiscontinuousConvMode = DISABLE;
  sConfigInjected.InjectedOffset = 0;
  if (HAL_ADCEx_InjectedConfigChannel(&hadc1, &sConfigInjected) != HAL_OK) Error_Handler();
}

/**
  * @brief ETH Initialization Function
  */
static void MX_ETH_Init(void)
{
   static uint8_t MACAddr[6];
  heth.Instance = ETH;
  MACAddr[0] = 0x00; MACAddr[1] = 0x80; MACAddr[2] = 0xE1;
  MACAddr[3] = 0x00; MACAddr[4] = 0x00; MACAddr[5] = 0x00;
  heth.Init.MACAddr = &MACAddr[0];
  heth.Init.MediaInterface = HAL_ETH_RMII_MODE;
  heth.Init.TxDesc = DMATxDscrTab;
  heth.Init.RxDesc = DMARxDscrTab;
  heth.Init.RxBuffLen = 1524;
  if (HAL_ETH_Init(&heth) != HAL_OK) Error_Handler();

  memset(&TxConfig, 0 , sizeof(ETH_TxPacketConfig));
  TxConfig.Attributes = ETH_TX_PACKETS_FEATURES_CSUM | ETH_TX_PACKETS_FEATURES_CRCPAD;
  TxConfig.ChecksumCtrl = ETH_CHECKSUM_IPHDR_PAYLOAD_INSERT_PHDR_CALC;
  TxConfig.CRCPadCtrl = ETH_CRC_PAD_INSERT;
}

/**
  * @brief TIM2 Initialization Function (Preserved from legacy IOC)
  */
static void MX_TIM2_Init(void)
{
  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 0;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 4499; // 216MHz / 4500 = 48000 Hz EXACT
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK) Error_Handler();

  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK) Error_Handler();

  sMasterConfig.MasterOutputTrigger = TIM_TRGO_UPDATE;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK) Error_Handler();
}

/**
  * @brief USART2 Initialization Function (PRESERVED - UNUSED BUT HARMLESS)
  */
static void MX_USART2_UART_Init(void)
{
  huart2.Instance = USART2;
  huart2.Init.BaudRate = 2000000;
  huart2.Init.WordLength = UART_WORDLENGTH_8B;
  huart2.Init.StopBits = UART_STOPBITS_1;
  huart2.Init.Parity = UART_PARITY_NONE;
  huart2.Init.Mode = UART_MODE_TX_RX;
  huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart2.Init.OverSampling = UART_OVERSAMPLING_16;
  huart2.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart2.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart2) != HAL_OK) Error_Handler();
}

/**
  * @brief USART3 Initialization Function (ACTIVE UART OUTPUT)
  */
static void MX_USART3_UART_Init(void)
{
  huart3.Instance = USART3;
  huart3.Init.BaudRate = 2000000;
  huart3.Init.WordLength = UART_WORDLENGTH_8B;
  huart3.Init.StopBits = UART_STOPBITS_1;
  huart3.Init.Parity = UART_PARITY_NONE;
  huart3.Init.Mode = UART_MODE_TX_RX;
  huart3.Init.HwFlowCtl = UART_HWCONTROL_NONE;
  huart3.Init.OverSampling = UART_OVERSAMPLING_8;
  huart3.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
  huart3.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
  if (HAL_UART_Init(&huart3) != HAL_OK) Error_Handler();
}

/**
  * @brief USB_OTG_FS Initialization Function
  */
static void MX_USB_OTG_FS_PCD_Init(void)
{
  hpcd_USB_OTG_FS.Instance = USB_OTG_FS;
  hpcd_USB_OTG_FS.Init.dev_endpoints = 6;
  hpcd_USB_OTG_FS.Init.speed = PCD_SPEED_FULL;
  hpcd_USB_OTG_FS.Init.dma_enable = DISABLE;
  hpcd_USB_OTG_FS.Init.phy_itface = PCD_PHY_EMBEDDED;
  hpcd_USB_OTG_FS.Init.Sof_enable = ENABLE;
  hpcd_USB_OTG_FS.Init.low_power_enable = DISABLE;
  hpcd_USB_OTG_FS.Init.lpm_enable = DISABLE;
  hpcd_USB_OTG_FS.Init.vbus_sensing_enable = ENABLE;
  hpcd_USB_OTG_FS.Init.use_dedicated_ep1 = DISABLE;
  if (HAL_PCD_Init(&hpcd_USB_OTG_FS) != HAL_OK) Error_Handler();
}

/**
  * Enable DMA controller clock
  */
static void MX_DMA_Init(void)
{
  __HAL_RCC_DMA2_CLK_ENABLE();
  __HAL_RCC_DMA1_CLK_ENABLE();

  HAL_NVIC_SetPriority(DMA1_Stream3_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA1_Stream3_IRQn);
  HAL_NVIC_SetPriority(DMA2_Stream0_IRQn, 0, 0);
  HAL_NVIC_EnableIRQ(DMA2_Stream0_IRQn);
}

/**
  * @brief GPIO Initialization Function
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};

  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOH_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();
  __HAL_RCC_GPIOG_CLK_ENABLE();

  HAL_GPIO_WritePin(GPIOB, LD1_Pin|LD3_Pin|LD2_Pin, GPIO_PIN_RESET);
  HAL_GPIO_WritePin(USB_PowerSwitchOn_GPIO_Port, USB_PowerSwitchOn_Pin, GPIO_PIN_RESET);

  GPIO_InitStruct.Pin = USER_Btn_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_IT_RISING;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(USER_Btn_GPIO_Port, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = LD1_Pin|LD3_Pin|LD2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = USB_PowerSwitchOn_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(USB_PowerSwitchOn_GPIO_Port, &GPIO_InitStruct);

  GPIO_InitStruct.Pin = USB_OverCurrent_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(USB_OverCurrent_GPIO_Port, &GPIO_InitStruct);
}

/* USER CODE BEGIN 4 */
static void MPU_Config(void)
{
  MPU_Region_InitTypeDef MPU_InitStruct = {0};
  HAL_MPU_Disable();

  MPU_InitStruct.Enable = MPU_REGION_ENABLE;
  MPU_InitStruct.Number = MPU_REGION_NUMBER0;
  MPU_InitStruct.BaseAddress = 0x20020000;
  MPU_InitStruct.Size = MPU_REGION_SIZE_512KB;
  MPU_InitStruct.SubRegionDisable = 0x00;
  MPU_InitStruct.TypeExtField = MPU_TEX_LEVEL1;
  MPU_InitStruct.AccessPermission = MPU_REGION_FULL_ACCESS;
  MPU_InitStruct.DisableExec = MPU_INSTRUCTION_ACCESS_ENABLE;
  MPU_InitStruct.IsShareable = MPU_ACCESS_SHAREABLE;
  MPU_InitStruct.IsCacheable = MPU_ACCESS_NOT_CACHEABLE;
  MPU_InitStruct.IsBufferable = MPU_ACCESS_NOT_BUFFERABLE;

  HAL_MPU_ConfigRegion(&MPU_InitStruct);
  HAL_MPU_Enable(MPU_PRIVILEGED_DEFAULT);
}
/* USER CODE END 4 */

/**
  * @brief  Error Handler
  * @retval None
  */
void Error_Handler(void)
{
  __disable_irq();
  while (1) {}
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
