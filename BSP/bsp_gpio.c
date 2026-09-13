#include "bsp_gpio.h"
#include "main.h"

void BSP_LED_On(BSP_LED_t led) {
    if (led == BSP_LED_RUN) {
        // LED_EXT_1 on PC6 (Active High via Q500)
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_6, GPIO_PIN_SET);
    } else if (led == BSP_LED_POWER) {
        // LED_EXT_2 power indicator on PC7 (Active High via Q501)
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_7, GPIO_PIN_SET);
    }
}

void BSP_LED_Off(BSP_LED_t led) {
    if (led == BSP_LED_RUN) {
        HAL_GPIO_WritePin(GPIOC, GPIO_PIN_6, GPIO_PIN_RESET);
    }
}

bool BSP_BTN_IsPressed(BSP_BTN_t btn) {
    if (btn == BSP_BTN_START) {
        // BUTTON_1 on PA15. Passed through Schmitt trigger. Active Low or High?
        // Assuming Schmitt trigger outputs High when pressed for now.
        return (HAL_GPIO_ReadPin(GPIOA, GPIO_PIN_15) == GPIO_PIN_SET);
    } else if (btn == BSP_BTN_STOP) {
        // BUTTON_2 on PD2
        return (HAL_GPIO_ReadPin(GPIOD, GPIO_PIN_2) == GPIO_PIN_SET);
    }
    return false;
}
