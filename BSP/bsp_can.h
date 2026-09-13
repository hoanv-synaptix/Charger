#ifndef BSP_CAN_H
#define BSP_CAN_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t ext_id;
    uint8_t  data[8];
    uint8_t  dlc;
} BSP_CAN_Frame_t;

/* Raw frame captured from one FDCAN RX FIFO.  This type deliberately carries
 * no protocol meaning: decoding belongs to the consumer in main context. */
typedef struct {
    uint32_t id;
    bool     is_extended;
    uint8_t  data[8];
    uint8_t  dlc;
    uint32_t rx_tick;
} BSP_CAN_RxFrame_t;

typedef struct {
    uint32_t raw_rx_count;
    uint32_t queued_rx_count;
    uint32_t queue_overflow_count;
    uint32_t stale_queue_drop_count;
    uint32_t fifo_lost_count;
    uint32_t fifo_full_count;
    uint32_t bus_off_count;
    uint32_t error_warning_count;
    uint32_t error_passive_count;
    uint32_t protocol_error_count;
} BSP_CAN_RxStats_t;

/* Bus 1 = FDCAN1 (125Kbps, charger modules)
 * Bus 2 = FDCAN2 (250Kbps, BMS) */
bool BSP_CAN_Start(void);
bool BSP_CAN_Transmit(uint8_t bus, const BSP_CAN_Frame_t *frame);

void BSP_CAN_GetStats(uint32_t *c1tx, uint32_t *c1rx, uint32_t *c2tx, uint32_t *c2rx);

/* TX-FIFO-full counts (HAL_FDCAN_AddMessageToTxFifoQ() failures) per bus --
 * a stat counter, not a LOG(), because BSP_CAN_Transmit() can be called
 * from inside chg_lib's critical section (see bsp_can.c for the full
 * rationale). Poll this instead of expecting a log line. */
void BSP_CAN_GetTxFailStats(uint32_t *c1_fail, uint32_t *c2_fail);

/* Rx dispatch callbacks. BSP only captures raw frames off the wire; these
 * handlers are invoked by BSP_CAN_ProcessRx() from main context. */
typedef void (*BSP_CAN_ChargerRxHandler_t)(uint32_t ext_id, const uint8_t *data, uint8_t dlc);
typedef void (*BSP_CAN_BmsRxHandler_t)(uint32_t ext_id, uint32_t std_id, const uint8_t *data, uint8_t dlc);

void BSP_CAN_SetChargerRxHandler(BSP_CAN_ChargerRxHandler_t handler);
void BSP_CAN_SetBmsRxHandler(BSP_CAN_BmsRxHandler_t handler);

/* Deliver a bounded number of captured frames to protocol consumers. */
void BSP_CAN_ProcessRx(void);

/* Bench-only diagnostic: drain successful MCU -> charger-module control
 * frames to UART1. No-op unless CHG_DEBUG_CAN_TX_TRACE is enabled. */
void BSP_CAN_ProcessTxTrace(void);

/* Read transport diagnostics.  bus is 1 for FDCAN1 or 2 for FDCAN2. */
void BSP_CAN_GetRxStats(uint8_t bus, BSP_CAN_RxStats_t *stats);

/* Watchdog bus-off: kiem tra PSR moi 1s, restart controller neu bus-off.
 * Goi lien tuc trong App_Loop. */
void BSP_CAN_Process(void);

#endif /* BSP_CAN_H */
