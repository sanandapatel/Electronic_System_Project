################################################################################
# Automatically-generated file. Do not edit!
# Toolchain: GNU Tools for STM32 (13.3.rel1)
################################################################################

# Add inputs and outputs from these tool invocations to the build variables 
C_SRCS += \
../Drivers/CMSIS/DSP/Source/ComplexMathFunctions/arm_cmplx_mag_f32.c 

OBJS += \
./Drivers/CMSIS/DSP/Source/ComplexMathFunctions/arm_cmplx_mag_f32.o 

C_DEPS += \
./Drivers/CMSIS/DSP/Source/ComplexMathFunctions/arm_cmplx_mag_f32.d 


# Each subdirectory must supply rules for building sources it contributes
Drivers/CMSIS/DSP/Source/ComplexMathFunctions/%.o Drivers/CMSIS/DSP/Source/ComplexMathFunctions/%.su Drivers/CMSIS/DSP/Source/ComplexMathFunctions/%.cyclo: ../Drivers/CMSIS/DSP/Source/ComplexMathFunctions/%.c Drivers/CMSIS/DSP/Source/ComplexMathFunctions/subdir.mk
	arm-none-eabi-gcc "$<" -mcpu=cortex-m4 -std=gnu11 -g3 -DDEBUG -DARM_MATH_CM4 -DUSE_NUCLEO_64 -DUSE_HAL_DRIVER -DSTM32G474xx -c -I../Core/Inc -I../Drivers/CMSIS/DSP/Include -I../Drivers/STM32G4xx_HAL_Driver/Inc -I../Drivers/STM32G4xx_HAL_Driver/Inc/Legacy -I../Drivers/BSP/STM32G4xx_Nucleo -I../Drivers/CMSIS/Device/ST/STM32G4xx/Include -I../Drivers/CMSIS/Include -O0 -ffunction-sections -fdata-sections -Wall -fstack-usage -fcyclomatic-complexity -MMD -MP -MF"$(@:%.o=%.d)" -MT"$@" --specs=nano.specs -mfpu=fpv4-sp-d16 -mfloat-abi=hard -mthumb -o "$@"

clean: clean-Drivers-2f-CMSIS-2f-DSP-2f-Source-2f-ComplexMathFunctions

clean-Drivers-2f-CMSIS-2f-DSP-2f-Source-2f-ComplexMathFunctions:
	-$(RM) ./Drivers/CMSIS/DSP/Source/ComplexMathFunctions/arm_cmplx_mag_f32.cyclo ./Drivers/CMSIS/DSP/Source/ComplexMathFunctions/arm_cmplx_mag_f32.d ./Drivers/CMSIS/DSP/Source/ComplexMathFunctions/arm_cmplx_mag_f32.o ./Drivers/CMSIS/DSP/Source/ComplexMathFunctions/arm_cmplx_mag_f32.su

.PHONY: clean-Drivers-2f-CMSIS-2f-DSP-2f-Source-2f-ComplexMathFunctions

