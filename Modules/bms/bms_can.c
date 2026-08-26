#include "bms_can.h"
#include "bsp_can.h"

bool BMS_CAN_Init(void) {
    return true; // Filter and Start is done in BSP_CAN_Start()
}

bool BMS_CAN_Transmit(uint32_t ext_id, const uint8_t *data, uint8_t len) {
    BSP_CAN_Frame_t frame;
    frame.ext_id = ext_id;
    frame.dlc = len;
    for (int i=0; i<8; i++) frame.data[i] = (i < len) ? data[i] : 0;
    return BSP_CAN_Transmit(2, &frame);
}
