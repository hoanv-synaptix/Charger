#ifndef BSP_ADC_H
#define BSP_ADC_H

#include <stdint.h>

void BSP_ADC_Init(void);
float BSP_ADC_GetTempC(uint8_t channel_index);

#endif
