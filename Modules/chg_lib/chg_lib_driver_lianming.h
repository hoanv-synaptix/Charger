/**
 * @file chg_lib_driver_lianming.h
 * @brief Lianming Power Digital Charging Module Driver Interface
 * @note Protocol V2.0 - CAN 2.0B Extended Frame, 125Kbps
 *
 * Usage:
 *   1. Register driver: CHG_LIB_RegisterDriver(CHG_LIB_DRV_LIANMING, CHG_LIB_LianmingDriverOps());
 *   2. Select driver: CHG_LIB_SelectDriver(CHG_LIB_DRV_LIANMING);
 *   3. Initialize: CHG_LIB_Init();
 *   4. Add module: CHG_LIB_AddModule(module_addr, group);
 *
 * For detailed protocol specification, see driver_lianming.c
 */

#ifndef CHG_LIB_DRV_LIANMING_H
#define CHG_LIB_DRV_LIANMING_H

#include "chg_lib.h"

/**
 * @brief Get driver operations table for Lianming module
 * @return Pointer to CHG_LIB_DriverOps_t
 */
const CHG_LIB_DriverOps_t *CHG_LIB_LianmingDriverOps(void);

#endif /* DRIVER_LIANMING_H */
