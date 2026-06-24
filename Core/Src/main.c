/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Real-Time Spectrum Analyzer with UART DMA Output
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "arm_math.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include <math.h>
#include <string.h>
#include <stdio.h>
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN TD */

/* ---- Spectrum Analyzer Configuration ---- */
#define FFT_SIZE 1024U
#define FFT_HALF (FFT_SIZE / 2U)
#define DMA_BUF_SIZE (FFT_SIZE * 2U)
#define PACKET_HEADER 0xA55AA55AU

typedef struct __attribute__((packed)) {
    uint32_t header;
    uint32_t frame_id;
    uint16_t fft_bins;
    uint16_t reserved;
    float magnitude[FFT_HALF];
} FFTPacket_t;

/* USER CODE END TD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

ADC_HandleTypeDef hadc1;
DMA_HandleTypeDef hdma_adc1;
DMA_HandleTypeDef hdma_usart2_tx;
TIM_HandleTypeDef htim6;
UART_HandleTypeDef huart2;

/* USER CODE BEGIN PV */

/* DMA Double Buffer */
static uint16_t adc_dma_buf[DMA_BUF_SIZE] __attribute__((aligned(4)));

/* FFT Processing Buffers */
static float32_t fft_input[FFT_SIZE];
static float32_t fft_output[FFT_SIZE * 2];
float32_t mag_buf[FFT_HALF];

/* Hann Window */
static float32_t hann_window[FFT_SIZE];

/* CMSIS-DSP FFT Instance */
static arm_rfft_fast_instance_f32 fft_instance;

/* Processing Flags */
static volatile uint8_t half_ready = 0;
static volatile uint8_t full_ready = 0;

/* UART TX DMA busy flag */
volatile uint8_t uart_tx_busy = 0;

/* Frame counter */
static uint32_t frame_id = 0;

/* Test pattern flag */
static uint8_t test_mode = 1;  /* Set to 0 for real ADC data */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
static void MX_GPIO_Init(void);
static void MX_DMA_Init(void);
static void MX_ADC1_Init(void);
static void MX_TIM6_Init(void);
static void MX_USART2_Init(void);
static void Generate_Hann_Window(void);
static void Process_FFT_Block(const uint16_t *samples);
static void Send_FFT_Packet(void);
static void Generate_Test_Signal(void);

/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* ============================================================================
 *  HANN WINDOW GENERATION
 * ==========================================================================*/
static void Generate_Hann_Window(void) {
    for (uint32_t n = 0; n < FFT_SIZE; n++) {
        hann_window[n] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)n / (float)(FFT_SIZE - 1)));
    }
}

/* ============================================================================
 *  GENERATE TEST SIGNAL (for debugging without ADC)
 * ==========================================================================*/
static void Generate_Test_Signal(void) {
    static uint16_t phase = 0;
    static uint32_t sample_count = 0;

    /* Generate a sine wave at 10% of sampling rate */
    for (uint32_t i = 0; i < FFT_SIZE; i++) {
        float val = 2048.0f + 1024.0f * sinf(2.0f * (float)M_PI * 100.0f * (float)i / FFT_SIZE);
        adc_dma_buf[sample_count++] = (uint16_t)val;
        if (sample_count >= DMA_BUF_SIZE) {
            sample_count = 0;
            /* Trigger processing */
            if (half_ready == 0) {
                half_ready = 1;
            }
        }
    }
}

/* ============================================================================
 *  FFT PROCESSING
 * ==========================================================================*/
static void Process_FFT_Block(const uint16_t *samples) {
    float32_t sum = 0.0f;

    /* Convert to float and calculate mean */
    for (uint32_t i = 0; i < FFT_SIZE; i++) {
        fft_input[i] = (float32_t)samples[i];
        sum += fft_input[i];
    }

    /* DC removal */
    float32_t dc_mean = sum / (float32_t)FFT_SIZE;

    /* Apply Hann window */
    for (uint32_t i = 0; i < FFT_SIZE; i++) {
        fft_input[i] = (fft_input[i] - dc_mean) * hann_window[i];
    }

    /* FFT */
    arm_rfft_fast_f32(&fft_instance, fft_input, fft_output, 0);

    /* Magnitude calculation */
    mag_buf[0] = fabsf(fft_output[0]);
    arm_cmplx_mag_f32(&fft_output[2], &mag_buf[1], FFT_HALF - 1);
}

/* ============================================================================
 *  SEND FFT PACKET VIA UART DMA
 * ==========================================================================*/
static void Send_FFT_Packet(void) {
    static FFTPacket_t packet;

    /* Only send if UART is not busy */
    if (uart_tx_busy == 0) {
        /* Fill packet */
        packet.header = PACKET_HEADER;
        packet.frame_id = frame_id++;
        packet.fft_bins = FFT_HALF;
        packet.reserved = 0;

        /* Copy magnitude data */
        memcpy(packet.magnitude, mag_buf, sizeof(float) * FFT_HALF);

        /* Start UART DMA transmission */
        uart_tx_busy = 1;
        if (HAL_UART_Transmit_DMA(&huart2, (uint8_t*)&packet, sizeof(FFTPacket_t)) != HAL_OK) {
            uart_tx_busy = 0;  /* Reset on error */
        }
    }
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void) {
    /* USER CODE BEGIN 1 */

    /* USER CODE END 1 */

    /* MCU Configuration--------------------------------------------------------*/
    HAL_Init();

    /* USER CODE BEGIN Init */

    /* USER CODE END Init */

    /* Configure the system clock */
    SystemClock_Config();

    /* USER CODE BEGIN SysInit */

    /* USER CODE END SysInit */

    /* Initialize all configured peripherals */
    MX_GPIO_Init();
    MX_DMA_Init();
    MX_ADC1_Init();
    MX_TIM6_Init();
    MX_USART2_Init();

    /* USER CODE BEGIN 2 */

    /* CMSIS-DSP: initialize FFT */
    arm_rfft_fast_init_f32(&fft_instance, FFT_SIZE);

    /* Pre-compute Hann window */
    Generate_Hann_Window();

    /* Send startup message */
    uint8_t startup_msg[] = "STM32 FFT UART READY\r\n";
    HAL_UART_Transmit(&huart2, startup_msg, sizeof(startup_msg) - 1, 1000);

    /* Start ADC DMA */
    HAL_ADC_Start_DMA(&hadc1, (uint32_t *)adc_dma_buf, DMA_BUF_SIZE);

    /* Start TIM6 */
    HAL_TIM_Base_Start(&htim6);

    /* USER CODE END 2 */
    uint8_t test_msg[] = "UART TEST\r\n";

    /* Infinite loop */
    while (1) {

        HAL_UART_Transmit(&huart2, test_msg, sizeof(test_msg)-1, 1000);
        HAL_Delay(1000);
        /* USER CODE BEGIN WHILE */

        /* Process first half */
        if (half_ready) {
            half_ready = 0;
            Process_FFT_Block(&adc_dma_buf[0]);
            Send_FFT_Packet();
        }

        /* Process second half */
        if (full_ready) {
            full_ready = 0;
            Process_FFT_Block(&adc_dma_buf[FFT_SIZE]);
            Send_FFT_Packet();
        }

        /* Generate test signal if in test mode */
        if (test_mode) {
            Generate_Test_Signal();
            HAL_Delay(10);
        }






        /* USER CODE END WHILE */
    }
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void) {
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1_BOOST);

    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
    RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
    RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV4;
    RCC_OscInitStruct.PLL.PLLN = 85;
    RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
    RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
    RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
    if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK) {
        Error_Handler();
    }

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK
                                | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
    RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;

    if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_8) != HAL_OK) {
        Error_Handler();
    }
}

/**
  * @brief ADC1 Initialization Function
  */
static void MX_ADC1_Init(void) {
    ADC_MultiModeTypeDef multimode = {0};
    ADC_ChannelConfTypeDef sConfig = {0};

    hadc1.Instance = ADC1;
    hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
    hadc1.Init.Resolution = ADC_RESOLUTION_12B;
    hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
    hadc1.Init.GainCompensation = 0;
    hadc1.Init.ScanConvMode = ADC_SCAN_DISABLE;
    hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
    hadc1.Init.LowPowerAutoWait = DISABLE;
    hadc1.Init.ContinuousConvMode = DISABLE;
    hadc1.Init.NbrOfConversion = 1;
    hadc1.Init.DiscontinuousConvMode = DISABLE;
    hadc1.Init.ExternalTrigConv = ADC_EXTERNALTRIG_T6_TRGO;
    hadc1.Init.ExternalTrigConvEdge = ADC_EXTERNALTRIGCONVEDGE_RISING;
    hadc1.Init.DMAContinuousRequests = ENABLE;
    hadc1.Init.Overrun = ADC_OVR_DATA_OVERWRITTEN;
    if (HAL_ADC_Init(&hadc1) != HAL_OK) {
        Error_Handler();
    }

    multimode.Mode = ADC_MODE_INDEPENDENT;
    if (HAL_ADCEx_MultiModeConfigChannel(&hadc1, &multimode) != HAL_OK) {
        Error_Handler();
    }

    sConfig.Channel = ADC_CHANNEL_1;
    sConfig.Rank = ADC_REGULAR_RANK_1;
    sConfig.SamplingTime = ADC_SAMPLETIME_2CYCLES_5;
    sConfig.SingleDiff = ADC_SINGLE_ENDED;
    sConfig.OffsetNumber = ADC_OFFSET_NONE;
    sConfig.Offset = 0;
    if (HAL_ADC_ConfigChannel(&hadc1, &sConfig) != HAL_OK) {
        Error_Handler();
    }

    if (HAL_ADCEx_Calibration_Start(&hadc1, ADC_SINGLE_ENDED) != HAL_OK) {
        Error_Handler();
    }
}

/**
  * @brief TIM6 Initialization Function
  */
static void MX_TIM6_Init(void) {
    TIM_MasterConfigTypeDef sMasterConfig = {0};

    htim6.Instance = TIM6;
    htim6.Init.Prescaler = 0;
    htim6.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim6.Init.Period = 1699;
    htim6.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    if (HAL_TIM_Base_Init(&htim6) != HAL_OK) {
        Error_Handler();
    }

    sMasterConfig.MasterOutputTrigger = TIM_TRGO_UPDATE;
    sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
    if (HAL_TIMEx_MasterConfigSynchronization(&htim6, &sMasterConfig) != HAL_OK) {
        Error_Handler();
    }
}

/**
  * @brief USART2 Initialization Function
  */
static void MX_USART2_Init(void) {
    huart2.Instance = USART2;
    huart2.Init.BaudRate = 921600;
    huart2.Init.WordLength = UART_WORDLENGTH_8B;
    huart2.Init.StopBits = UART_STOPBITS_1;
    huart2.Init.Parity = UART_PARITY_NONE;
    huart2.Init.Mode = UART_MODE_TX;
    huart2.Init.HwFlowCtl = UART_HWCONTROL_NONE;
    huart2.Init.OverSampling = UART_OVERSAMPLING_16;
    huart2.Init.OneBitSampling = UART_ONE_BIT_SAMPLE_DISABLE;
    huart2.Init.ClockPrescaler = UART_PRESCALER_DIV1;
    huart2.AdvancedInit.AdvFeatureInit = UART_ADVFEATURE_NO_INIT;
    if (HAL_UART_Init(&huart2) != HAL_OK) {
        Error_Handler();
    }
}

/**
  * @brief DMA Initialization Function
  */
static void MX_DMA_Init(void) {
    __HAL_RCC_DMAMUX1_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();

    HAL_NVIC_SetPriority(DMA1_Channel1_IRQn, 0, 0);
    HAL_NVIC_EnableIRQ(DMA1_Channel1_IRQn);

    HAL_NVIC_SetPriority(DMA1_Channel6_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(DMA1_Channel6_IRQn);

    HAL_NVIC_SetPriority(USART2_IRQn, 1, 0);
    HAL_NVIC_EnableIRQ(USART2_IRQn);
}

/**
  * @brief GPIO Initialization Function
  */
static void MX_GPIO_Init(void) {
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();

    /* PA0 - ADC input */
    GPIO_InitStruct.Pin = GPIO_PIN_0;
    GPIO_InitStruct.Mode = GPIO_MODE_ANALOG;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
}

/* USER CODE BEGIN 4 */

/* ============================================================================
 *  DMA CALLBACKS
 * ==========================================================================*/
void HAL_ADC_ConvHalfCpltCallback(ADC_HandleTypeDef *hadc) {
    if (hadc->Instance == ADC1) {
        half_ready = 1;
    }
}

void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef *hadc) {
    if (hadc->Instance == ADC1) {
        full_ready = 1;
    }
}

/* ============================================================================
 *  UART CALLBACKS
 * ==========================================================================*/
void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart) {
    if (huart->Instance == USART2) {
        uart_tx_busy = 0;
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart) {
    if (huart->Instance == USART2) {
        uart_tx_busy = 0;
        HAL_UART_Abort(&huart2);
    }
}

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void) {
    /* USER CODE BEGIN Error_Handler_Debug */
    __disable_irq();
    while (1) {
    }
    /* USER CODE END Error_Handler_Debug */
}

#ifdef USE_FULL_ASSERT
void assert_failed(uint8_t *file, uint32_t line) {
    /* USER CODE BEGIN 6 */
    /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
