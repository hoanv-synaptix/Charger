/**
  * @file stm32g0xx_hal_iwdg.h
  * @brief Minimal stub for IWDG - direct register implementation used instead of HAL
  */
#ifndef STM32G0xx_HAL_IWDG_H
#define STM32G0xx_HAL_IWDG_H

#ifdef __cplusplus
extern "C" {
#endif

#include "stm32g0xx_hal_def.h"

typedef struct
{
  IWDG_TypeDef *Instance;
  struct {
    uint32_t Prescaler;
    uint32_t Window;
    uint32_t Reload;
  } Init;
} IWDG_HandleTypeDef;

#define IWDG_PRESCALER_4    0U
#define IWDG_PRESCALER_8    1U
#define IWDG_PRESCALER_16   2U
#define IWDG_PRESCALER_32   3U
#define IWDG_PRESCALER_64   4U
#define IWDG_PRESCALER_128  5U
#define IWDG_PRESCALER_256  6U
#define IWDG_WINDOW_DISABLE 4095U

HAL_StatusTypeDef HAL_IWDG_Init(IWDG_HandleTypeDef *hiwdg);
HAL_StatusTypeDef HAL_IWDG_Refresh(IWDG_HandleTypeDef *hiwdg);

#ifdef __cplusplus
}
#endif

#endif /* STM32G0xx_HAL_IWDG_H */
