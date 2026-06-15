#ifndef USER_ECU_MAIN_H
#define USER_ECU_MAIN_H

/*
 * main.h — C-runtime entry of the user_ecu firmware.
 *
 * Reconstructed from user_ecu.20240129.145222.1.5.0.main.v1.5.0-main.bin
 * (ARM Cortex-M4F, ARMv7-M VFPv4 hard-float, FreeRTOS). Image base 0x0.
 */

/*
 * main_SystemInit — system bring-up, called by Reset_Handler (@0x1d4).
 * // 0x000044c0
 *
 * Brings up clocks, GPIO pin-mux and peripheral clock-gates; configures the
 * LED-PWM (FTM), ADC, the I2C/IOM bus and the SAI/DMIC path inline; creates the
 * FreeRTOS tasks via xTaskCreate; programs SHPR3 and a 1 ms SysTick; then starts
 * the scheduler (vPortStartFirstTask) and never returns.
 */
void main_SystemInit(void);

#endif /* USER_ECU_MAIN_H */
