#include "bsp_sys.h"
#include "main.h"

uint32_t BSP_GetTick(void) {
    return HAL_GetTick();
}

void BSP_Delay(uint32_t delay_ms) {
    HAL_Delay(delay_ms);
}
