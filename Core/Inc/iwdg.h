/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file    iwdg.h
  * @brief   Header for iwdg.c
  ******************************************************************************
  */
/* USER CODE END Header */
#ifndef __IWDG_H__
#define __IWDG_H__

#ifdef __cplusplus
extern "C" {
#endif

#include "main.h"

void MX_IWDG_Init(void);
void MX_IWDG_Refresh(void);

#ifdef __cplusplus
}
#endif

#endif /* __IWDG_H__ */
