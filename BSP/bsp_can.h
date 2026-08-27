#ifndef BSP_CAN_H
#define BSP_CAN_H

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t ext_id;
    uint8_t  data[8];
    uint8_t  dlc;
} BSP_CAN_Frame_t;

/* Bus 1 = FDCAN1 (125Kbps, charger modules)
 * Bus 2 = FDCAN2 (250Kbps, BMS) */
bool BSP_CAN_Start(void);
bool BSP_CAN_Transmit(uint8_t bus, const BSP_CAN_Frame_t *frame);

void BSP_CAN_GetStats(uint32_t *c1tx, uint32_t *c1rx, uint32_t *c2tx, uint32_t *c2rx);

/* Rx dispatch callbacks. BSP only captures frames off the wire; it must not
 * know about charger/BMS business modules (AGENTS.md sec 5-6). The
 * composition root (App_Init) registers these once at startup; the FDCAN
 * RX ISR (HAL_FDCAN_RxFifo0Callback, bsp_can.c) invokes whichever is set
 * for the bus the frame arrived on. Handlers run in ISR context exactly as
 * before this refactor -- this only removes the compile-time dependency,
 * it does not change when or where frames are processed. */
typedef void (*BSP_CAN_ChargerRxHandler_t)(uint32_t ext_id, const uint8_t *data, uint8_t dlc);
typedef void (*BSP_CAN_BmsRxHandler_t)(uint32_t ext_id, uint32_t std_id, const uint8_t *data, uint8_t dlc);

void BSP_CAN_SetChargerRxHandler(BSP_CAN_ChargerRxHandler_t handler);
void BSP_CAN_SetBmsRxHandler(BSP_CAN_BmsRxHandler_t handler);

/* Watchdog bus-off: kiem tra PSR moi 1s, restart controller neu bus-off.
 * Goi lien tuc trong App_Loop. */
void BSP_CAN_Process(void);

#endif /* BSP_CAN_H */
