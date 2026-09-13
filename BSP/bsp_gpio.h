#ifndef BSP_GPIO_H
#define BSP_GPIO_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    BSP_LED_RUN,
    BSP_LED_POWER /* PC7: power indicator, enabled by App_Init */
} BSP_LED_t;

typedef enum {
    BSP_BTN_START,
    BSP_BTN_STOP
} BSP_BTN_t;

void BSP_LED_On(BSP_LED_t led);
void BSP_LED_Off(BSP_LED_t led);
bool BSP_BTN_IsPressed(BSP_BTN_t btn);

#endif
