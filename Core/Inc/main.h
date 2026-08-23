/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32g0xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define MCU_PC13_LED_STT_Pin GPIO_PIN_13
#define MCU_PC13_LED_STT_GPIO_Port GPIOC
#define NTC_ADC0_Pin GPIO_PIN_0
#define NTC_ADC0_GPIO_Port GPIOA
#define NTC_ADC1_Pin GPIO_PIN_1
#define NTC_ADC1_GPIO_Port GPIOA
#define NTC_ADC2_Pin GPIO_PIN_2
#define NTC_ADC2_GPIO_Port GPIOA
#define NTC_ADC3_Pin GPIO_PIN_3
#define NTC_ADC3_GPIO_Port GPIOA
#define MCU_PA4_POWER_EN_Pin GPIO_PIN_4
#define MCU_PA4_POWER_EN_GPIO_Port GPIOA
#define MCU_PA5_SPI1_CLK_Pin GPIO_PIN_5
#define MCU_PA5_SPI1_CLK_GPIO_Port GPIOA
#define MCU_PA6_SPI1_MISO_Pin GPIO_PIN_6
#define MCU_PA6_SPI1_MISO_GPIO_Port GPIOA
#define MCU_PA7_SPI1_MOSI_Pin GPIO_PIN_7
#define MCU_PA7_SPI1_MOSI_GPIO_Port GPIOA
#define MCU_PB0_SPI1_CS_Pin GPIO_PIN_0
#define MCU_PB0_SPI1_CS_GPIO_Port GPIOB
#define MCU_PB1_UART_RTS_Pin GPIO_PIN_1
#define MCU_PB1_UART_RTS_GPIO_Port GPIOB
#define MCU_PB2_MicroSD_DT_Pin GPIO_PIN_2
#define MCU_PB2_MicroSD_DT_GPIO_Port GPIOB
#define MCU_PB10_USART3_TX_Pin GPIO_PIN_10
#define MCU_PB10_USART3_TX_GPIO_Port GPIOB
#define MCU_PB11_USART3_RX_Pin GPIO_PIN_11
#define MCU_PB11_USART3_RX_GPIO_Port GPIOB
#define MCU_PB14_RELAY_1_Pin GPIO_PIN_14
#define MCU_PB14_RELAY_1_GPIO_Port GPIOB
#define MCU_PB15_RELAY_2_Pin GPIO_PIN_15
#define MCU_PB15_RELAY_2_GPIO_Port GPIOB
#define MCU_PA8_RELAY_3_Pin GPIO_PIN_8
#define MCU_PA8_RELAY_3_GPIO_Port GPIOA
#define MCU_PA9_USART1_TX_Pin GPIO_PIN_9
#define MCU_PA9_USART1_TX_GPIO_Port GPIOA
#define MCU_PC6_LED_EXT_1_Pin GPIO_PIN_6
#define MCU_PC6_LED_EXT_1_GPIO_Port GPIOC
#define MCU_PC7_LED_EXT_2_Pin GPIO_PIN_7
#define MCU_PC7_LED_EXT_2_GPIO_Port GPIOC
#define MCU_PA10_USART1_RX_Pin GPIO_PIN_10
#define MCU_PA10_USART1_RX_GPIO_Port GPIOA
#define MCU_PA11_USB_DM_Pin GPIO_PIN_11
#define MCU_PA11_USB_DM_GPIO_Port GPIOA
#define MCU_PA12_USB_DP_Pin GPIO_PIN_12
#define MCU_PA12_USB_DP_GPIO_Port GPIOA
#define MCU_PA15_BUTTON_1_Pin GPIO_PIN_15
#define MCU_PA15_BUTTON_1_GPIO_Port GPIOA
#define MCU_PD2_BUTTON_2_Pin GPIO_PIN_2
#define MCU_PD2_BUTTON_2_GPIO_Port GPIOD
#define MCU_PD3_LTE_PWRKEY_Pin GPIO_PIN_3
#define MCU_PD3_LTE_PWRKEY_GPIO_Port GPIOD
#define MCU_PB3_USART5_TX_Pin GPIO_PIN_3
#define MCU_PB3_USART5_TX_GPIO_Port GPIOB
#define MCU_PB4_USART5_RX_Pin GPIO_PIN_4
#define MCU_PB4_USART5_RX_GPIO_Port GPIOB
#define MCU_PB5_LTE_PWR_EN_Pin GPIO_PIN_5
#define MCU_PB5_LTE_PWR_EN_GPIO_Port GPIOB
#define MCU_PB6_SPI2_MISO_Pin GPIO_PIN_6
#define MCU_PB6_SPI2_MISO_GPIO_Port GPIOB
#define MCU_PB7_SPI2_MOSI_Pin GPIO_PIN_7
#define MCU_PB7_SPI2_MOSI_GPIO_Port GPIOB
#define MCU_PB8_SPI2_SCK_Pin GPIO_PIN_8
#define MCU_PB8_SPI2_SCK_GPIO_Port GPIOB
#define MCU_PB9_SPI2_CS_Pin GPIO_PIN_9
#define MCU_PB9_SPI2_CS_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */

/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
