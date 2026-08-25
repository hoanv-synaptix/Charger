#include "bsp_failsafe.h"
#include "main.h"
#include "fdcan.h"

void Safety_Shutdown(void)
{
    /* Turn off relays PB14, PB15, PA8 (active low/high? Use RESET to ensure off) */
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_14, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_15, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_8, GPIO_PIN_RESET);
    /* Lower POWER_EN PA4 */
    HAL_GPIO_WritePin(GPIOA, GPIO_PIN_4, GPIO_PIN_RESET);
    /* Stop CAN controllers to avoid bus flooding */
    HAL_FDCAN_Stop(&hfdcan1);
    HAL_FDCAN_Stop(&hfdcan2);
    /* Turn on fault LEDs to indicate safe state */
    HAL_GPIO_WritePin(GPIOC, GPIO_PIN_7, GPIO_PIN_SET);
}
