#ifndef SIM_CAN_MODULES_H
#define SIM_CAN_MODULES_H
/**
 * @file sim_can_modules.h
 * @brief Host-side charger-module simulators (Maxwell/Lianming/TonHe).
 *
 * Each simulator plays the role of the real module on the wire: it is
 * installed as the CHG_LIB_CanBackend_t.transmit callback (see
 * Modules/chg_lib/chg_lib_can_backend.c) so every outgoing CAN1 frame the
 * real firmware driver builds is handed to it instead of real hardware.
 * Byte layouts here are taken directly from the firmware source
 * (chg_lib_maxwell.c / chg_lib_lianming.c / chg_lib_tonhe.c), not
 * reinvented -- see docs/AUDIT and the plan this test was built from.
 *
 * Usage per scenario:
 *   sim_module_reset(&g_sim_module, addr, group);
 *   CHG_LIB_CanBackend_Set(&g_maxwell_backend);   // or lianming/tonhe
 *   ... drive CHG_LIB_Process()/ChargeController_Process() in a loop,
 *       calling sim_module_tick(&g_sim_module, driver, mock_tick) each
 *       iteration to let the simulator respond/broadcast.
 */
#include <stdint.h>
#include <stdbool.h>
#include "chg_lib.h"
#include "chg_lib_can_backend.h"

typedef enum {
    SIM_DRV_MAXWELL = 0,
    SIM_DRV_LIANMING,
    SIM_DRV_TONHE,
} SimDriverKind_t;

typedef struct {
    uint8_t addr;
    uint8_t group;

    /* What the simulated module reports */
    bool     actually_on;
    float    voltage;
    float    current;
    float    rated_current;
    float    rated_power;

    /* Driver-specific raw alarm/fault bits to report on the NEXT frame the
     * simulator sends -- set these from a test scenario, then call
     * sim_module_tick() to have them show up in the firmware's parsed
     * CHG_LIB_ModuleView_t.alarm_flags. */
    uint32_t maxwell_alarm_raw;   /* 32-bit, parse_maxwell_alarm() bit layout */
    uint16_t lianming_status_raw; /* 16-bit, parse_lianming_alarm() bit layout */
    uint16_t tonhe_fault_bits;    /* 16-bit, tonhe_parse_fault() byte6-7 layout */
    uint8_t  tonhe_pfc_bits;      /* 8-bit, tonhe_parse_fault() PFC byte layout */

    /* Internal: pending request captured by the transmit callback,
     * consumed by sim_module_tick() (Maxwell/Lianming request/response). */
    bool     pending;
    uint8_t  pending_func;   /* Maxwell func code, or Lianming cmd byte */
    uint16_t pending_reg;    /* Maxwell register */

    /* Last CHG_LIB_REG_SET_CURR_LIMIT ratio (0.0-1.0) the firmware wrote --
     * captured verbatim off the wire so a test can check the driver
     * computed the right ratio for the module's *configured* rated
     * current, not a fallback default (see chg_lib_maxwell.c's
     * rated_current_or_fallback() and the Maxwell protocol PDF sec 2.3.1:
     * "current limit = required / rated"). 0.0f (via sim_module_reset's
     * memset) until the first write -- callers that need to tell "never
     * written" apart from "written as exactly 0" should check `pending`
     * transitions instead; no scenario here needs that distinction. */
    float    last_set_curr_limit_ratio;
} SimModuleState_t;

/* The single module instance under test (host test is single-module by
 * design -- see plan's scenario list). Global so the C-linkage transmit
 * callbacks (which CHG_LIB_CanBackend_t requires as plain function
 * pointers, no closure) can reach it. */
extern SimModuleState_t g_sim_module;

void sim_module_reset(SimModuleState_t *m, uint8_t addr, uint8_t group);

/* Install the given driver's simulated CAN backend (replaces the real BSP
 * one) -- call once per scenario, before ChargeCycleConfig_Set() selects
 * the matching driver. */
void sim_install_backend(SimDriverKind_t kind);

/* Advance the simulator by one control-loop tick: Maxwell/Lianming answer
 * any pending request; TonHe broadcasts its periodic status frame. Call
 * this once per simulated ~20ms step, after CHG_LIB_Process(). */
void sim_module_tick(SimDriverKind_t kind, uint32_t now_tick);

#endif
