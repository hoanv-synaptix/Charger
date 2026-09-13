/**
 * @file chg_lib_driver_tonhe.h
 * @brief TonHe V1.3 Charging Module Driver Interface - J1939-based CAN protocol
 * @note Protocol: SAE J1939-21, CAN 2.0B Extended Frame, 125Kbps, isolated CAN
 *
 * Usage:
 *   1. Register driver: CHG_LIB_RegisterDriver(CHG_LIB_DRV_TONHE, CHG_LIB_TonheDriverOps());
 *   2. Select driver: CHG_LIB_SelectDriver(CHG_LIB_DRV_TONHE);
 *   3. Initialize: CHG_LIB_Init();
 *   4. Add module: CHG_LIB_AddModule(module_addr, group);
 *
 * For detailed protocol specification, see driver_tonhe.c
 */

#ifndef CHG_LIB_DRV_TONHE_H
#define CHG_LIB_DRV_TONHE_H

#include "chg_lib.h"

/* ============== CAN Address ============== */

#define TONHE_ADDR_CONTROLLER   0xA0U
#define TONHE_ADDR_BROADCAST  0xFFU
#define TONHE_MODULE_MIN_ADDR  1
#define TONHE_MODULE_MAX_ADDR 240

/* ============== PGN (Parameter Group Numbers) ============== */
/* Uplink: Module -> Master */
#define TONHE_PGN_STATUS      0x000100U  /* M_C_1: Charging module status */
#define TONHE_PGN_CONFIRM    0x000200U  /* M_C_2: Start/stop confirm */
#define TONHE_PGN_AC_PHASE   0x000B00U  /* M_C_3: AC phase information */
#define TONHE_PGN_EXTENDED   0x009100U  /* M_C_4: Extended status/fault */

/* Downlink: Master -> Module */
#define TONHE_PGN_BROADCAST_CMD   0x000300U  /* C_M_1: Broadcast start/stop */
#define TONHE_PGN_PARAM_SET       0x000400U  /* C_M_2: Broadcast parameter */
#define TONHE_PGN_TIMING          0x000500U  /* C_M_3: Timing command */
#define TONHE_PGN_SPECIFIC_CMD    0x000600U  /* C_M_24: Specific module start/stop */
#define TONHE_PGN_ADDR_SET        0x000900U  /* C_M_23: Address setting */
#define TONHE_PGN_ADDR_MODE       0x009000U  /* C_M_12: Address mode selection */

/* ============== Priority ============== */

#define TONHE_PRIORITY_STATUS     6   /* M_C_1, M_C_3, M_C_4 */
#define TONHE_PRIORITY_PARAM      4   /* C_M_2: Broadcast parameter setting */
#define TONHE_PRIORITY_BROADCAST  2   /* C_M_1: Broadcast start/stop */
#define TONHE_PRIORITY_CMD        2   /* C_M_24: Specific start/stop */
#define TONHE_PRIORITY_TIMING     6   /* C_M_3: Timing command */
#define TONHE_PRIORITY_EXTENDED   7   /* M_C_4: Extended status/fault */

/* ============== Scale Factors ============== */

#define TONHE_VOLTAGE_SCALE   0.1f      /* V per bit */
#define TONHE_AC_LINE_VOLTAGE_SCALE  0.173f  /* V per bit: 0.1V/bit * 1.73 (phase-to-line conversion) */
#define TONHE_CURRENT_SCALE   0.01f     /* A per bit */
#define TONHE_TEMP_SCALE      1.0f      /* °C per bit */

/* ============== Limits ============== */

#define TONHE_MAX_OUTPUT_VOLTAGE_V  750.0f
#define TONHE_MAX_OUTPUT_CURRENT_A 500.0f
#define TONHE_MAX_MODULES          8

/* ============== Timeouts ============== */

#define TONHE_WARNING_TIMEOUT_MS    2000U
#define TONHE_OFFLINE_TIMEOUT_MS    10000U
#define TONHE_RECOVERY_DELAY_MS   3000U
#define TONHE_CONFIRM_TIMEOUT_MS   1000U
#define TONHE_POLL_INTERVAL_MS     20U
#define TONHE_HEARTBEAT_INTERVAL_MS 1000U
#define TONHE_MAX_RETRY            3U    /* Max retries before giving up */

/* ============== Command Values ============== */

#define TONHE_CMD_STOP    0x55U
#define TONHE_CMD_START  0xAAU
#define TONHE_MODE_STANDBY 0x00U

/* ============== Status Values ============== */

#define TONHE_STATUS_NORMAL_OFF  0x00U
#define TONHE_STATUS_ON        0x01U
#define TONHE_STATUS_FAULT_OFF 0x11U

/* ============== API ============== */

/**
 * @brief Get driver operations table for TONHE module
 * @return Pointer to CHG_LIB_DriverOps_t
 */
const CHG_LIB_DriverOps_t *CHG_LIB_TonheDriverOps(void);

#endif /* DRIVER_TONHE_H */
