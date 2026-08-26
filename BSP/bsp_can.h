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

/* Watchdog bus-off: kiem tra PSR moi 1s, restart controller neu bus-off.
 * Goi lien tuc trong App_Loop. */
void BSP_CAN_Process(void);

#endif /* BSP_CAN_H */
