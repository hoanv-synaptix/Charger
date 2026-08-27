#ifndef MOCK_BSP_SYS_H
#define MOCK_BSP_SYS_H
#include <stdint.h>
uint32_t BSP_GetTick(void);
void BSP_Delay(uint32_t delay_ms);
void BSP_EnterCritical(void);
void BSP_ExitCritical(void);
#endif
