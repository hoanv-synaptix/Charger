/**
 * @file chg_lib_core_priv.h
 * @brief Shared private helpers for charger drivers
 * @note Internal header — not exported by the library.
 *       Provides FSM helpers, module lookup, and summary accumulation
 *       used by all driver implementations.
 */

#ifndef CHG_LIB_CORE_PRIV_H
#define CHG_LIB_CORE_PRIV_H

#include <stdint.h>
#include <stdbool.h>
#include "chg_lib.h"
#include "chg_lib_can_backend.h"

/* ============================================================
 * Debug Logging (optional)
 * @note Application defines LOG() via debug_log.h if needed.
 *       If not defined, LOG becomes a no-op for library builds.
 * ============================================================ */
#ifndef LOG
#define LOG(...) do { } while (0)
#endif

/* ============================================================
 * System Tick
 * ============================================================ */

/**
 * @brief Get current system tick via CAN backend
 * @note Replaces the 3 identical now_tick() local functions
 */
static inline uint32_t CHG_LIB_NowTick(void)
{
    return CHG_LIB_CanBackend_NowTick();
}

/* ============================================================
 * Module Lookup
 * ============================================================ */

/**
 * @brief Find module index by CAN address
 * @param modules Array of module views
 * @param count Number of modules
 * @param addr CAN address to find
 * @return Module index or 0xFF if not found
 */
static inline uint8_t CHG_LIB_FindByAddr(
    const CHG_LIB_ModuleView_t *modules,
    uint8_t count,
    uint8_t addr)
{
    for (uint8_t i = 0; i < count; i++) {
        if (modules[i].addr == addr) {
            return i;
        }
    }
    return 0xFFU;
}

/* ============================================================
 * Summary Accumulation
 * @brief Accumulate module data into system summary
 * @param summary Pointer to summary to accumulate into
 * @param v Module view to add
 * @param extra_power_in Additional input power (for drivers that calculate differently)
 * @note Does NOT accumulate max_temp_dcdc — each driver handles that differently
 * ============================================================ */
void CHG_LIB_Summary_Accumulate(
    CHG_LIB_SystemSummary_t *summary,
    const CHG_LIB_ModuleView_t *v,
    float extra_power_in);

/* ============================================================
 * FSM State Transition
 * @brief Set module state with transition tracking
 * @param p_state Current state pointer
 * @param p_last_tick Last state change tick pointer
 * @param p_attempts Start attempt counter (may be NULL if driver doesn't track)
 * @param new_state Target state
 * @param now Current system tick
 * @note Handles transitions for all 3 drivers. TonHe passes NULL for p_attempts.
 * ============================================================ */
void CHG_LIB_FSM_SetState(
    CHG_LIB_State_t *p_state,
    uint32_t *p_last_tick,
    uint8_t *p_attempts,
    CHG_LIB_State_t new_state,
    uint32_t now);

void CHG_LIB_FSM_CheckOfflineTimeout(
    const CHG_LIB_State_t *p_state,
    uint32_t last_rx_tick,
    bool should_run,
    uint32_t offline_timeout_ms,
    uint32_t warning_timeout_ms,
    uint32_t now,
    CHG_LIB_State_t *p_new_state,
    bool *p_timeout_flag);

#endif /* CHG_LIB_CORE_PRIV_H */
