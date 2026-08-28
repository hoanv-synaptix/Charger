#ifndef MOCK_MAIN_H
#define MOCK_MAIN_H
/* charge_controller.c includes main.h but never references a GPIO symbol
 * from it (verified by grep) -- only needs HAL_GetTick's prototype, which
 * a real main.h would pull in transitively via stm32g0xx_hal.h. */
#include "stm32g0xx_hal.h"
#endif
