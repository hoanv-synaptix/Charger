#ifndef BSP_SYS_H
#define BSP_SYS_H

#include <stdint.h>

uint32_t BSP_GetTick(void);
void BSP_Delay(uint32_t delay_ms);

/* Minimal critical-section wrapper (disable/enable IRQ). Exists so Modules/
 * App code can protect a shared variable against ISR access without
 * including a CMSIS/HAL header just to reach __disable_irq/__enable_irq
 * (AGENTS.md sec 5-6: only Platform may depend on the MCU/HAL directly).
 * Not reentrant/nestable -- matches the __disable_irq()/__enable_irq() pairs
 * it replaces, which were not nestable either. */
void BSP_EnterCritical(void);
void BSP_ExitCritical(void);

#endif
