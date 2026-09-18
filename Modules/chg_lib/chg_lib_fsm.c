/**
 * @file chg_lib_fsm.c
 * @brief Shared FSM helpers and summary accumulation
 * @note Implements CHG_LIB_FSM_SetState() and CHG_LIB_Summary_Accumulate()
 */

#include "priv/chg_lib_core_priv.h"
#include <stddef.h>
#include <math.h>

#define FSM_RECOVERY_DELAY_MS  3000U

void CHG_LIB_FSM_SetState(
    CHG_LIB_State_t *p_state,
    uint32_t *p_last_tick,
    uint8_t *p_attempts,
    CHG_LIB_State_t new_state,
    uint32_t now)
{
    if (p_state == NULL) return;

    CHG_LIB_State_t prev = *p_state;
    if (prev == new_state) return;

    *p_state = new_state;
    if (p_last_tick != NULL) {
        *p_last_tick = now;
    }

    if (new_state == CHG_LIB_STATE_STARTING && p_attempts != NULL) {
        (*p_attempts)++;
    }
}

void CHG_LIB_Summary_Accumulate(
    CHG_LIB_SystemSummary_t *summary,
    const CHG_LIB_ModuleView_t *v,
    float extra_power_in)
{
    if (summary == NULL || v == NULL || !v->enabled) return;

    if (v->online) {
        summary->modules_online++;
    }

    if (v->state == CHG_LIB_STATE_FAULT) {
        summary->modules_fault++;
        summary->any_critical = true;
    }
    if (v->alarm_flags != 0) {
        summary->any_critical = true;
    }

    /* Electrical values are live telemetry, not last-known values. An
     * offline module may retain its previous current/voltage in the view;
     * excluding it here keeps every summary consumer from making decisions
     * from stale output data. */
    if (!v->online) return;

    if (isfinite(v->current) && v->current >= 0.0f) {
        summary->total_current += v->current;
    }

    float power_in = extra_power_in;
    if (extra_power_in == 0.0f && v->input_power > 0U) {
        power_in = (float)v->input_power;
    }
    if (isfinite(power_in) && power_in >= 0.0f) {
        summary->total_power_in += power_in;
    }

    if (isfinite(v->voltage) && v->voltage > 0.1f) {
        summary->voltage = v->voltage;
    }
}

void CHG_LIB_FSM_CheckOfflineTimeout(
    const CHG_LIB_State_t *p_state,
    uint32_t last_rx_tick,
    bool should_run,
    uint32_t offline_timeout_ms,
    uint32_t warning_timeout_ms,
    uint32_t now,
    CHG_LIB_State_t *p_new_state,
    bool *p_timeout_flag)
{
    *p_new_state = *p_state;
    *p_timeout_flag = false;
    if (*p_state == CHG_LIB_STATE_OFFLINE || *p_state == CHG_LIB_STATE_RECOVERING) {
        return;
    }

    /* BUGFIX 2026-08-29: this watchdog used to run unconditionally, so a
     * module the operator had already told to stop (should_run cleared by
     * xxx_stop()) could still get dragged from IDLE into
     * WARNING/OFFLINE/RECOVERING purely because nothing had polled it
     * recently -- a comms-health concept that only makes sense while the
     * charger actually needs this module (mid-cycle, or trying to start
     * one). Confirmed with the user: OFFLINE/RECOVERING should represent
     * an unplanned comms loss *during* an active session, not "we haven't
     * bothered listening to it since STOP" -- an operator-issued
     * STOP/EMERGENCY_STOP is a deliberate action, its result should read
     * as IDLE, not a comms-health state that only a future reconnect can
     * clear. Only run the timeout-driven OFFLINE demotion while the
     * module is actually wanted (should_run) -- xxx_stop() (in each
     * driver) is the counterpart fix: it now force-transitions a stopped
     * module straight to IDLE from WARNING/OFFLINE/RECOVERING instead of
     * only handling RUNNING/STARTING, so a module already parked in one of
     * those comms-health states when STOP is pressed doesn't just sit
     * there un-reachable by this now-gated watchdog. */
    if (!should_run) {
        return;
    }

    uint32_t since_rx = now - last_rx_tick;

    if (since_rx > offline_timeout_ms) {
        *p_timeout_flag = true;
        *p_new_state = CHG_LIB_STATE_OFFLINE;
    } else if (since_rx > warning_timeout_ms) {
        if (*p_state == CHG_LIB_STATE_RUNNING || *p_state == CHG_LIB_STATE_STARTING) {
            *p_new_state = CHG_LIB_STATE_WARNING;
        }
    } else {
        if (*p_state == CHG_LIB_STATE_WARNING) {
            *p_new_state = should_run ? CHG_LIB_STATE_RUNNING : CHG_LIB_STATE_IDLE;
        }
    }
}
