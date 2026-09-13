/**
 * @file chg_lib_can_backend.h
 * @brief   CAN Backend Abstraction for Charger Drivers
 * @note    Allows charger drivers to be independent of BSP CAN implementation.
 *           Production: BSP provides the backend (transmits via HAL CAN)
 *           Testing: Mock backend (captures TX frames for verification)
 *
 * Architecture:
 *   ┌─────────────────┐     ┌──────────────────────────┐
 *   │  Charger Driver │────▶│  CHG_LIB_CanBackend_t        │
 *   └─────────────────┘     │  - transmit()            │
 *                           │  - now_tick()            │
 *                           └──────────┬───────────────┘
 *                                      │ implements
 *           ┌───────────────────────────┴────────────────────┐
 *           ▼                                                 ▼
 *   ┌───────────────┐                               ┌──────────────┐
 *   │  BSP Backend  │ (production)                 │  Mock       │ (testing)
 *   │  bsp_can.c    │                               │  test suite  │
 *   └───────────────┘                               └──────────────┘
 */

#ifndef CHG_LIB_CAN_BACKEND_H
#define CHG_LIB_CAN_BACKEND_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    CHG_LIB_TX_SOURCE_UNKNOWN = 0,
    CHG_LIB_TX_SOURCE_CC_START,
    CHG_LIB_TX_SOURCE_CC_INHIBIT,
    CHG_LIB_TX_SOURCE_CC_COMPLETION,
    CHG_LIB_TX_SOURCE_CC_STOP,
    CHG_LIB_TX_SOURCE_PC_SET_CURRENT,
    CHG_LIB_TX_SOURCE_PC_PROFILE,
    CHG_LIB_TX_SOURCE_DRIVER_RECOVERY,
    CHG_LIB_TX_SOURCE_CC_RAMP,
    CHG_LIB_TX_SOURCE_COUNT
} CHG_LIB_TxSource_t;

typedef enum {
    CHG_LIB_TX_REASON_UNKNOWN = 0,
    CHG_LIB_TX_REASON_NORMAL,
    CHG_LIB_TX_REASON_HEARTBEAT,
    CHG_LIB_TX_REASON_START_RESET,
    CHG_LIB_TX_REASON_CURRENT_ZERO,
    CHG_LIB_TX_REASON_STOP,
    CHG_LIB_TX_REASON_COUNT
} CHG_LIB_TxReason_t;

typedef enum {
    CHG_LIB_CURRENT_PATH_UNKNOWN = 0,
    CHG_LIB_CURRENT_PATH_ALL_EX,
    CHG_LIB_CURRENT_PATH_MODULE_EX,
    CHG_LIB_CURRENT_PATH_LEGACY_ALL,
    CHG_LIB_CURRENT_PATH_LEGACY_MODULE,
    CHG_LIB_CURRENT_PATH_PC_SET_CURRENT,
    CHG_LIB_CURRENT_PATH_PC_PROFILE,
    CHG_LIB_CURRENT_PATH_CONTROLLER,
    CHG_LIB_CURRENT_PATH_COUNT
} CHG_LIB_CurrentPath_t;

/**
 * @brief CAN backend interface for charger drivers
 * @note Drivers call CHG_CanTransmit() which delegates to the registered backend
 */
typedef struct {
    /** Transmit a CAN frame */
    bool (*transmit)(uint32_t ext_id, const uint8_t *data, uint8_t dlc);
    /** Get current system tick (for timeout tracking) */
    uint32_t (*now_tick)(void);
} CHG_LIB_CanBackend_t;

/**
 * @brief Register the CAN backend (called once at startup)
 * @param backend Pointer to backend implementation (must remain valid)
 */
void CHG_LIB_CanBackend_Set(const CHG_LIB_CanBackend_t *backend);

/**
 * @brief Initialize with BSP backend (call once at startup)
 */
void CHG_LIB_CanBackend_Init(void);

/**
 * @brief Get current system tick via backend
 * @return Current tick value
 */
uint32_t CHG_LIB_CanBackend_NowTick(void);

/**
 * @brief Transmit CAN frame via backend
 * @retval true if frame was queued for transmission
 */
bool CHG_LIB_CanBackend_Transmit(uint32_t ext_id, const uint8_t *data, uint8_t dlc);
bool CHG_LIB_CanBackend_TransmitEx(uint32_t ext_id, const uint8_t *data, uint8_t dlc,
                                   CHG_LIB_TxSource_t source);
bool CHG_LIB_CanBackend_TransmitMeta(uint32_t ext_id, const uint8_t *data, uint8_t dlc,
                                     CHG_LIB_TxSource_t source,
                                     CHG_LIB_TxReason_t reason);

#endif /* CHG_CAN_BACKEND_H */
