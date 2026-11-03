#include "bsp_gpio.h"
#include "main.h"

void BSP_LED_On(BSP_LED_t led) {
    if (led == BSP_LED_RUN) {
        // LED_EXT_1 on PC6 (Active High via Q500)
        HAL_GPIO_WritePin(MCU_PC6_LED_EXT_1_GPIO_Port, MCU_PC6_LED_EXT_1_Pin, GPIO_PIN_SET);
    } else if (led == BSP_LED_POWER) {
        // LED_EXT_2 power indicator on PC7 (Active High via Q501)
        HAL_GPIO_WritePin(MCU_PC7_LED_EXT_2_GPIO_Port, MCU_PC7_LED_EXT_2_Pin, GPIO_PIN_SET);
    }
}

void BSP_LED_Off(BSP_LED_t led) {
    if (led == BSP_LED_RUN) {
        HAL_GPIO_WritePin(MCU_PC6_LED_EXT_1_GPIO_Port, MCU_PC6_LED_EXT_1_Pin, GPIO_PIN_RESET);
    } else if (led == BSP_LED_POWER) {
        HAL_GPIO_WritePin(MCU_PC7_LED_EXT_2_GPIO_Port, MCU_PC7_LED_EXT_2_Pin, GPIO_PIN_RESET);
    }
}

bool BSP_BTN_IsPressed(BSP_BTN_t btn) {
    if (btn == BSP_BTN_START) {
        // BUTTON_1 on PA15
        return (HAL_GPIO_ReadPin(MCU_PA15_BUTTON_1_GPIO_Port, MCU_PA15_BUTTON_1_Pin) == GPIO_PIN_SET);
    } else if (btn == BSP_BTN_STOP) {
        // BUTTON_2 on PD2
        return (HAL_GPIO_ReadPin(MCU_PD2_BUTTON_2_GPIO_Port, MCU_PD2_BUTTON_2_Pin) == GPIO_PIN_SET);
    }
    return false;
}

bool BSP_IsReversePolarityDetected(void) {
    /* Optocoupler U1 (PC817XI) conducts when reverse polarity occurs,
     * pulling MCU_PA8_REVS_DET down to GND (LOW).
     * Normal connection or no battery: pulled HIGH via R7 (10k). */
    return (HAL_GPIO_ReadPin(MCU_PA8_REVS_DET_GPIO_Port, MCU_PA8_REVS_DET_Pin) == GPIO_PIN_RESET);
}
