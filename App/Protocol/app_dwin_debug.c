/**
 * @file app_dwin_debug.c
 * @brief See app_dwin_debug.h. Built only when CHG_DEBUG_DWIN is defined.
 *
 * Concurrency: QueueTx()/TakeRx() run in USB-ISR context; Pump()/CaptureRx()
 * run in the main loop. Cortex-M0+ is single-core and byte/word loads/stores
 * are atomic, and the ISR can preempt the main loop but not vice-versa. The
 * fields are `volatile` and the flags are written last / read first so a
 * torn buffer is never *acted on*. A second PC request landing mid-copy can
 * still interleave debug bytes across two poll responses -- acceptable for a
 * bench tool the operator drives one request at a time, and not worth a
 * critical section here (App/Protocol must not reach into BSP).
 */
#include "app_dwin_debug.h"

#ifdef CHG_DEBUG_DWIN

#include <string.h>

/* Same platform seam Modules/hmi/dwin_protocol.c uses (resolved by the
 * linker; no BSP include so the layering check stays clean). */
extern void UART_Transmit_To_DWIN(uint8_t *data, uint16_t len);

#define DWIN_DBG_TX_MAX  40U
#define DWIN_DBG_RX_MAX  96U

static volatile uint8_t s_tx[DWIN_DBG_TX_MAX];
static volatile uint8_t s_tx_len = 0;
static volatile uint8_t s_tx_pending = 0;

static volatile uint8_t s_rx[DWIN_DBG_RX_MAX];
static volatile uint8_t s_rx_len = 0;

void AppDwinDebug_QueueTx(const uint8_t *data, uint8_t len)
{
    if (data == 0 || len == 0U || len > DWIN_DBG_TX_MAX) {
        return;
    }
    for (uint8_t i = 0; i < len; i++) {
        s_tx[i] = data[i];
    }
    s_tx_len = len;
    s_rx_len = 0U;             /* drop stale RX before this exchange */
    s_tx_pending = 1U;         /* written last -> Pump only acts on a full buffer */
}

void AppDwinDebug_Pump(void)
{
    uint8_t local[DWIN_DBG_TX_MAX];
    uint8_t n;

    if (!s_tx_pending) {
        return;
    }
    n = s_tx_len;
    for (uint8_t i = 0; i < n; i++) {
        local[i] = s_tx[i];
    }
    s_tx_pending = 0U;

    UART_Transmit_To_DWIN(local, n);   /* main-loop ctx: blocking HAL call is fine */
}

void AppDwinDebug_CaptureRx(const uint8_t *data, uint16_t len)
{
    if (data == 0) {
        return;
    }
    for (uint16_t i = 0; (i < len) && (s_rx_len < DWIN_DBG_RX_MAX); i++) {
        s_rx[s_rx_len] = data[i];
        s_rx_len++;
    }
}

uint8_t AppDwinDebug_TakeRx(uint8_t *out, uint8_t max)
{
    uint8_t n;

    if (out == 0) {
        return 0U;
    }
    n = (s_rx_len < max) ? s_rx_len : max;
    for (uint8_t i = 0; i < n; i++) {
        out[i] = s_rx[i];
    }
    s_rx_len = 0U;
    return n;
}

#endif /* CHG_DEBUG_DWIN */
