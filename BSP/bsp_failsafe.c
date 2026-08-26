#include "bsp_failsafe.h"
#include "main.h"
#include "fdcan.h"

extern FDCAN_HandleTypeDef hfdcan1;
extern FDCAN_HandleTypeDef hfdcan2;

void Safety_Shutdown(void)
{
    /* Use direct register access — HAL may not be safe if called from
     * HardFault or if peripheral clocks are not yet enabled. */
    /* Turn off relays PB14, PB15, PA8 */
    GPIOB->BSRR = (GPIO_PIN_14 | GPIO_PIN_15) << 16u;
    GPIOA->BSRR = GPIO_PIN_8 << 16u;
    /* Lower POWER_EN PA4 */
    GPIOA->BSRR = GPIO_PIN_4 << 16u;
    /* Stop CAN controllers only if they were initialized */
    if (hfdcan1.Instance != NULL) {
        HAL_FDCAN_Stop(&hfdcan1);
    }
    if (hfdcan2.Instance != NULL) {
        HAL_FDCAN_Stop(&hfdcan2);
    }
    /* Turn on fault LED PC7 */
    GPIOC->BSRR = GPIO_PIN_7;
}
