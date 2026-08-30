/**
 * @file  app_dwin_debug.h
 * @brief Bench-debug bridge: lets the PC (via the USB-CDC debug protocol,
 *        DEBUG_CMD_DWIN_XFER) push a raw frame out the RS485 line to the
 *        DWIN panel and read back whatever the panel replied -- using the
 *        MCU's own transceiver, no re-wiring.
 *
 * Compiled in only for Debug builds (CHG_DEBUG_DWIN, set in CMakeLists.txt).
 * In Release the calls below are inline no-ops so app_main.c needs no
 * #ifdef.
 *
 * The USB debug command handler runs in USB-ISR context, where
 * UART_Transmit_To_DWIN() (HAL_Delay + blocking HAL_UART_Transmit) must NOT
 * be called. So the handler only *queues* the bytes; App_Loop() drains the
 * queue in main-loop context and captures the RS485 RX in parallel with the
 * normal DWIN_ParseRX() path.
 */
#ifndef APP_DWIN_DEBUG_H
#define APP_DWIN_DEBUG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CHG_DEBUG_DWIN

/** ISR ctx: stash up to 40 bytes to transmit and clear the RX capture. */
void AppDwinDebug_QueueTx(const uint8_t *data, uint8_t len);

/** Main-loop ctx: if a TX is queued, send it now via UART_Transmit_To_DWIN. */
void AppDwinDebug_Pump(void);

/** Main-loop ctx: append raw RS485 RX bytes to the capture buffer. */
void AppDwinDebug_CaptureRx(const uint8_t *data, uint16_t len);

/** ISR ctx: copy out (up to @p max) captured RX bytes and clear. Returns n. */
uint8_t AppDwinDebug_TakeRx(uint8_t *out, uint8_t max);

#else /* !CHG_DEBUG_DWIN -- no-ops */

static inline void AppDwinDebug_Pump(void) {}
static inline void AppDwinDebug_CaptureRx(const uint8_t *d, uint16_t l) { (void)d; (void)l; }

#endif

#ifdef __cplusplus
}
#endif

#endif /* APP_DWIN_DEBUG_H */
