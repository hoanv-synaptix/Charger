#ifndef BMS_CAN_H
#define BMS_CAN_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief Initialize CAN2 for BMS communication (250Kbps)
 * @retval true if successful
 */
bool BMS_CAN_Init(void);

bool BMS_CAN_Transmit(uint32_t ext_id, const uint8_t *data, uint8_t len);
#endif

