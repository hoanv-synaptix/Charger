/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
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
#include "main.h"
#include "adc.h"
#include "dma.h"
#include "fdcan.h"
#include "spi.h"
#include "tim.h"
#include "usart.h"
#include "usb_device.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "app_main.h"
#include "iwdg.h"
#include "bsp_failsafe.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */
  /* EARLY DIAGNOSTIC: Blink PC6 (RUN LED) immediately after HAL_Init
   * to confirm MCU is alive. If this LED never toggles, the issue is
   * in SystemClock_Config or startup, not in App_Init. */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  GPIOC->MODER &= ~(3u << (6 * 2));
  GPIOC->MODER |=  (1u << (6 * 2));   /* PC6 output */
  GPIOC->BSRR = (1u << 6);            /* PC6 HIGH */
  for(volatile uint32_t i = 0; i < 500000; i++); /* ~100ms delay */
  GPIOC->BSRR = (1u << (6 + 16));     /* PC6 LOW */
  for(volatile uint32_t i = 0; i < 500000; i++);

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_DMA_Init();
  MX_ADC1_Init();
  MX_SPI1_Init();
  MX_USART1_UART_Init();
  /* EARLY DIAGNOSTIC: Send immediate print via UART1 to confirm USART1 works
   * BEFORE any FDCAN or USB init. If this appears, boot is alive up to here. */
  {
      const char *early_msg = "\r\n[EARLY] UART1 alive - booting...\r\n";
      HAL_UART_Transmit(&huart1, (uint8_t *)early_msg, (uint16_t)35, 100);
  }
  MX_FDCAN1_Init();
  MX_FDCAN2_Init();
  /* EARLY DIAGNOSTIC: Confirm FDCAN inits passed without Error_Handler */
  {
      const char *fdcan_msg = "[EARLY] FDCAN1+2 init OK\r\n";
      HAL_UART_Transmit(&huart1, (uint8_t *)fdcan_msg, (uint16_t)27, 100);
  }
  MX_SPI2_Init();
  MX_USART3_UART_Init();
  MX_USART5_UART_Init();
  MX_TIM6_Init();
  MX_TIM7_Init();
  /* EARLY DIAGNOSTIC: Confirm we reached USB init */
  {
      const char *pre_usb = "[EARLY] About to init USB...\r\n";
      HAL_UART_Transmit(&huart1, (uint8_t *)pre_usb, (uint16_t)30, 100);
  }
  MX_USB_Device_Init();
  /* EARLY DIAGNOSTIC: Confirm USB init passed */
  {
      const char *post_usb = "[EARLY] USB init done\r\n";
      HAL_UART_Transmit(&huart1, (uint8_t *)post_usb, (uint16_t)23, 100);
  }
  /* USER CODE BEGIN 2 */
  App_Init();

  /* IWDG must start AFTER App_Init() to avoid reset during boot sequence */
  MX_IWDG_Init();

  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    App_Loop();
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

  /** Configure the main internal regulator output voltage
  */
  HAL_PWREx_ControlVoltageScaling(PWR_REGULATOR_VOLTAGE_SCALE1);

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI|RCC_OSCILLATORTYPE_HSI48;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSI48State = RCC_HSI48_ON;
  RCC_OscInitStruct.HSIDiv = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = RCC_PLLM_DIV1;
  RCC_OscInitStruct.PLL.PLLN = 8;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = RCC_PLLQ_DIV2;
  RCC_OscInitStruct.PLL.PLLR = RCC_PLLR_DIV2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  Safety_Shutdown();
  
  /* Khởi tạo lại UART1 thủ công để in lỗi nếu cần */
  __HAL_RCC_USART1_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  GPIO_InitStruct.Pin = GPIO_PIN_9;
  GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  GPIO_InitStruct.Alternate = GPIO_AF1_USART1;
  HAL_GPIO_Init(GPIOA, &GPIO_InitStruct);
  
  USART1->CR1 = USART_CR1_TE | USART_CR1_UE;
  USART1->BRR = (SystemCoreClock / 115200);
  
  const char *msg = "\r\n[CRASH] Error_Handler() CALLED!\r\n";
  for(int i=0; msg[i] != '\0'; i++) {
      while((USART1->ISR & USART_ISR_TXE_TXFNF) == 0) {}
      USART1->TDR = msg[i];
  }
  
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
