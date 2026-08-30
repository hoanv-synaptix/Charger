#ifndef BSP_ADC_H
#define BSP_ADC_H

#include <stdint.h>

void BSP_ADC_Init(void);

/**
 * @brief Re-arm the NTC ADC scan. Call once per control cycle from the
 *        composition root (App_Loop) with BSP_GetTick(). The ADC + DMA are
 *        single-shot (see bsp_adc.c); without this the readings freeze at
 *        their power-on values. Internally rate-limited, so calling it every
 *        loop iteration is fine.
 * @param now_tick Current BSP_GetTick() value
 */
void BSP_ADC_Process(uint32_t now_tick);

float BSP_ADC_GetTempC(uint8_t channel_index);

#endif
