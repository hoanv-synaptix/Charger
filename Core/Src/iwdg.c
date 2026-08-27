/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    iwdg.c
  * @brief   IWDG init - Independent Watchdog 1s (LSI ~32k, prescaler 32, reload 1000)
  * @note    Direct register implementation — avoids HAL_IWDG dependency
  ******************************************************************************
  */
/* USER CODE END Header */
#include "iwdg.h"
#include "main.h"

/**
  * @brief IWDG Initialization Function
  * @param None
  * @retval None
  */
void MX_IWDG_Init(void)
{
  /* 1. Bật LSI bằng thanh ghi (đề phòng Option Bytes tắt) */
  SET_BIT(RCC->CSR, RCC_CSR_LSION);

  /* 2. Đợi LSI ready với timeout (để không bị treo mãi mãi nếu LSI hỏng) */
  uint32_t start = HAL_GetTick();
  while ((RCC->CSR & RCC_CSR_LSIRDY) == 0U) {
      if ((HAL_GetTick() - start) > 100) {
          /* Timeout! Bỏ qua khởi tạo IWDG */
          return;
      }
  }

  /* 3. LSI đã chạy, bắt đầu config IWDG */

  /* USER CODE BEGIN IWDG_Init 0 */

  /* USER CODE END IWDG_Init 0 */

  /* Enable write access */
  IWDG->KR = 0x5555U;
  /* Prescaler 32: PR=3 */
  IWDG->PR = 3U;
  /* Reload 1000 → ~1s at 32k/32=1kHz */
  IWDG->RLR = 1000U;
  /* Window disabled (allow refresh anytime) */
  IWDG->WINR = 4095U;
  /* Wait for registers to update */
  while (IWDG->SR != 0U) { }
  /* Reload */
  IWDG->KR = 0xAAAAU;
  /* Start watchdog */
  IWDG->KR = 0xCCCCU;

  /* Freeze IWDG when core halted in debug — G0: DBGMCU APB1FZR1 */
#ifdef DBGMCU_APB1FZR1_DBG_IWDG_STOP
  SET_BIT(DBGMCU->APB1FZR1, DBGMCU_APB1FZR1_DBG_IWDG_STOP);
#endif
  /* USER CODE BEGIN IWDG_Init 2 */

  /* USER CODE END IWDG_Init 2 */
}

void MX_IWDG_Refresh(void)
{
  IWDG->KR = 0xAAAAU;
}
