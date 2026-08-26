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

#endif /* CHG_CAN_BACKEND_H */
