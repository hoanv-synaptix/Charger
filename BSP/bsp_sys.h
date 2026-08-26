#ifndef BSP_SYS_H
#define BSP_SYS_H

#include <stdint.h>

uint32_t BSP_GetTick(void);
void BSP_Delay(uint32_t delay_ms);

#endif
