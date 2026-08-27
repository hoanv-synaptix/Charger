/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    gpio.c
  * @brief   This file provides code for the configuration
  *          of all used GPIO pins.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "gpio.h"

/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/*----------------------------------------------------------------------------*/
/* Configure GPIO                                                             */
/*----------------------------------------------------------------------------*/
/* USER CODE BEGIN 1 */

/* USER CODE END 1 */

/** Configure pins as
        * Analog
        * Input
        * Output
        * EVENT_OUT
        * EXTI
*/
void MX_GPIO_Init(void)
{

  GPIO_InitTypeDef GPIO_InitStruct = {0};

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOF_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOC, MCU_PC13_LED_STT_Pin|MCU_PC6_LED_EXT_1_Pin|MCU_PC7_LED_EXT_2_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOA, MCU_PA4_POWER_EN_Pin|MCU_PA8_RELAY_3_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(GPIOB, MCU_PB0_SPI1_CS_Pin, GPIO_PIN_SET);

  /* I-06 fix: MCU_PB1_UART_RTS_Pin drives the RS485 DE line. Defaulting it
   * HIGH (TX enable) here -- before BSP_RS485_Init() explicitly sets it LOW
   * -- lets the transceiver briefly drive the RS485/DWIN bus during boot
   * (bus contention / glitch). Default it to RX (LOW) instead so the pin
   * never drives the bus until firmware is actually ready to transmit. */
  HAL_GPIO_WritePin(GPIOB, MCU_PB14_RELAY_1_Pin|MCU_PB15_RELAY_2_Pin|MCU_PB5_LTE_PWR_EN_Pin|MCU_PB9_SPI2_CS_Pin|MCU_PB1_UART_RTS_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(MCU_PD3_LTE_PWRKEY_GPIO_Port, MCU_PD3_LTE_PWRKEY_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pins : MCU_PC13_LED_STT_Pin MCU_PC6_LED_EXT_1_Pin MCU_PC7_LED_EXT_2_Pin */
  GPIO_InitStruct.Pin = MCU_PC13_LED_STT_Pin|MCU_PC6_LED_EXT_1_Pin|MCU_PC7_LED_EXT_2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOC, &GPIO_InitStruct);

  /*Configure GPIO pins : MCU_PA4_POWER_EN_Pin MCU_PA8_RELAY_3_Pin */
  GPIO_InitStruct.Pin = MCU_PA4_POWER_EN_Pin|MCU_PA8_RELAY_3_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);

  /*Configure GPIO pins : MCU_PB0_SPI1_CS_Pin MCU_PB1_UART_RTS_Pin MCU_PB14_RELAY_1_Pin MCU_PB15_RELAY_2_Pin
                           MCU_PB5_LTE_PWR_EN_Pin MCU_PB9_SPI2_CS_Pin */
  GPIO_InitStruct.Pin = MCU_PB0_SPI1_CS_Pin|MCU_PB1_UART_RTS_Pin|MCU_PB14_RELAY_1_Pin|MCU_PB15_RELAY_2_Pin
                          |MCU_PB5_LTE_PWR_EN_Pin|MCU_PB9_SPI2_CS_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

  /*Configure GPIO pin : MCU_PB2_MicroSD_DT_Pin */
  GPIO_InitStruct.Pin = MCU_PB2_MicroSD_DT_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_PULLUP;
  HAL_GPIO_Init(MCU_PB2_MicroSD_DT_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : MCU_PA15_BUTTON_1_Pin */
  GPIO_InitStruct.Pin = MCU_PA15_BUTTON_1_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(MCU_PA15_BUTTON_1_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : MCU_PD2_BUTTON_2_Pin */
  GPIO_InitStruct.Pin = MCU_PD2_BUTTON_2_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  HAL_GPIO_Init(MCU_PD2_BUTTON_2_GPIO_Port, &GPIO_InitStruct);

  /*Configure GPIO pin : MCU_PD3_LTE_PWRKEY_Pin */
  GPIO_InitStruct.Pin = MCU_PD3_LTE_PWRKEY_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(MCU_PD3_LTE_PWRKEY_GPIO_Port, &GPIO_InitStruct);

}

/* USER CODE BEGIN 2 */

/* USER CODE END 2 */
