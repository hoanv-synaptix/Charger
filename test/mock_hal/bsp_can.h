#ifndef MOCK_BSP_CAN_H
#define MOCK_BSP_CAN_H
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t ext_id;
    uint8_t  data[8];
    uint8_t  dlc;
    uint8_t  tx_source;
    uint8_t  tx_reason;
} BSP_CAN_Frame_t;

bool BSP_CAN_Transmit(uint8_t bus, const BSP_CAN_Frame_t *frame);
void BSP_CAN_GetStats(uint32_t *c1tx, uint32_t *c1rx, uint32_t *c2tx, uint32_t *c2rx);
void BSP_CAN_RecordTxTraceReject(uint8_t source, uint8_t path);

#endif
