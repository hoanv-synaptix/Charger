/**
 * @file charge_cycle_storage.h
 * @brief Flash persistence for ChargeCycleConfig_t
 */

#ifndef CHARGE_CYCLE_STORAGE_H
#define CHARGE_CYCLE_STORAGE_H

#include <stdbool.h>
#include <stdint.h>
#include "charge_cycle_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Initialize storage and load config from flash
 *        If flash is invalid, keeps RAM config at defaults
 */
void ChargeCycleStorage_Init(void);

/**
 * @brief Load config from flash into provided buffer
 * @param config Pointer to config buffer to fill
 * @return true if loaded successfully, false if flash invalid
 */
bool ChargeCycleStorage_Load(ChargeCycleConfig_t *config);

/**
 * @brief Save config to flash
 * @param config Pointer to config to save
 * @return true if saved successfully, false on error
 */
bool ChargeCycleStorage_Save(const ChargeCycleConfig_t *config);

#ifdef __cplusplus
}
#endif

#endif /* CHARGE_CYCLE_STORAGE_H */

