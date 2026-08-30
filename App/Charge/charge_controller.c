/**
 * @file charge_controller.c
 * @brief Charge cycle controller implementation
 *
 * Manages the charging state machine:
 * - IDLE: Not started
 * - READY: Start requested, checking preconditions
 * - RUNNING: Charging normally
 * - DERATING is retained as a protocol-compatible legacy value, but runtime
 *   derating is represented by g_ctrl.derating while state stays RUNNING.
 * - STOPPING: Controlled stop in progress
 * - FAULT: Error condition, charging stopped
 */

#include "charge_controller.h"
#include "charge_cycle_config.h"
#include "chg_lib.h"
#include "bms_core.h"
#include "debug_log.h"
#include <string.h>
#include <math.h>

/* Standalone completion policy: use fresh charger telemetry, not BMS data. */
#define CHARGE_CTRL_MODULE_VOLTAGE_MAX_AGE_MS       2000U
#define CHARGE_CTRL_STANDALONE_VMAX_CONFIRM_MS      1000U

/* Relay open-gating (2026-08-29): don't break a DC relay under load --
 * see update_relay_decision()'s docstring for the full rationale. Current
 * threshold is well above CAN quantization noise (TonHe's
 * TONHE_CURRENT_SCALE is 0.01A/bit) and small relative to typical 50-100A
 * module ratings. Timeout is a starting default, not yet validated against
 * real hardware ramp-down timing -- same caveat as this project's other
 * open timing constants (RS485 TX timeout, POWER_EN delay). */
#define RELAY_OPEN_CURRENT_THRESHOLD_A               1.0f
#define RELAY_OPEN_TIMEOUT_MS                        3000U

/* ============== Private State ============== */

static struct {
    ChargeCtrlState_t state;
    ChargeCtrlOwner_t owner;
    uint32_t fault_flags;
    uint32_t last_update_tick;

    /* Manual mode state */
    bool manual_mode;
    float manual_target_voltage_v;
    float manual_target_current_per_module_a;

    /* Targets */
    float target_voltage_v;
    float target_current_total_a;
    float target_current_per_module_a;

    /* Applied values */
    float applied_voltage_v;
    float applied_current_per_module_a;

    /* Stage evaluation */
    uint8_t inhibit;
    uint8_t derating;
    uint8_t active_limit_source;
    uint8_t active_stage_band;
    float active_limit_current_c;
    ChargeStopReason_t stop_reason;

    ChargeStageBand_t last_cell_band;
    ChargeStageBand_t last_temp_band;
    ChargeStageBand_t last_soc_band;

    /* Per-cycle monotonic progress for cell voltage and SOC. */
    ChargeStageBand_t max_cell_band;
    ChargeStageBand_t max_soc_band;
    bool cell_full_latched;
    bool soc_full_latched;

    /* Module count tracking */
    uint8_t source_module_count;
    uint8_t actual_module_count;

    /* Protection timers */
    uint32_t protect_jack_v_timer_tick;
    uint32_t protect_jack_temp_timer_tick;
    uint32_t module_mismatch_timer_tick;

    /* State change tracking for logging */
    uint8_t last_inhibit;
    uint8_t last_derating;
    uint8_t last_running;

    /* Jack temp soft derating flag */
    bool jack_temp_derating_active;

    /* BMS stale warning tracking */
    bool bms_stale_warned;

    /* Standalone voltage-completion confirmation timer */
    uint32_t standalone_vmax_reached_tick;

    /* Jack/connector temperature, in degrees C, supplied by the composition
     * root (App_Loop) once per control cycle via ChargeController_SetJackTempC().
     * Pure charging policy must not read BSP_ADC directly (AGENTS.md sec 5-6). */
    float jack_temp_input_c;

    /* Battery relay decision -- see ChargeCtrlView_t.relay_should_close and
     * update_relay_decision() for the full condition. relay_latched_closed
     * is the "has it already earned >=90% this RUNNING session" latch: once
     * set, voltage dropping back below 90% (normal charging behaviour, e.g.
     * CV-phase current taper) does NOT reopen the relay by itself -- only
     * leaving RUNNING or the BMS reporting unsafe does. Reset to false the
     * moment state leaves RUNNING, so the next RUNNING session must earn it
     * again. */
    bool relay_should_close;
    bool relay_latched_closed;

    /* 2026-08-29: don't drop relay_should_close the instant a fault/stop
     * is seen while latched closed -- wait for module current to settle
     * near zero (or time out) first, to avoid breaking a DC relay under
     * load. See update_relay_decision(). */
    bool relay_open_pending;
    uint32_t relay_open_pending_tick;

    /* Tracks the allow_charge value last sent to the BMS via
     * BMS_SendCtrlInfo() -- see update_bms_charge_allow(). Lets that
     * function send only on change instead of every tick. */
    bool bms_charge_allow_sent;
} g_ctrl = {0};

/* ============== Stage Evaluation Types ============== */

typedef enum {
    CHARGE_STAGE_IN_WINDOW = 0,
    CHARGE_STAGE_BELOW_MIN,
    CHARGE_STAGE_ABOVE_MAX,
} ChargeStageState_t;

typedef struct {
    uint8_t enabled;
    uint8_t inhibit;
    ChargeStageState_t state;
    ChargeStageBand_t band;
    ChargeLimitSource_t source;
    float current_limit_c;
} ChargeStageEval_t;

/* ============== Private Function Prototypes ============== */

static void set_fault(uint32_t flags, uint32_t now);
static void clear_fault(void);
static void update_relay_decision(uint32_t now_tick);
static float compute_voltage_ref(const ChargeCycleConfig_t *cfg);
static void update_bms_charge_allow(void);
static void transition_to(ChargeCtrlState_t new_state, uint32_t now);
static uint8_t get_active_module_count(void);
static bool check_preconditions_set_fault(uint32_t now);
static void apply_charge_targets(void);
static void stop_charging(void);

/* Stage evaluation */
static ChargeStageEval_t eval_cell_stage(const ChargeCycleConfig_t *cfg, const BMS_View_t *bms);
static ChargeStageEval_t eval_temp_stage(const ChargeCycleConfig_t *cfg, const BMS_View_t *bms);
static ChargeStageEval_t eval_soc_stage(const ChargeCycleConfig_t *cfg, const BMS_View_t *bms);
static bool compute_stage_limits(const ChargeCycleConfig_t *cfg, const BMS_View_t *bms,
                                 float *limit_c_out, uint8_t *inhibit_out,
                                 uint8_t *limit_source_out, uint8_t *stage_band_out);

/* Mode handlers */
static void run_standalone_mode(uint32_t now_tick);
static void run_bms_controlled_mode(uint32_t now_tick);
static bool check_standalone_voltage_reached(uint32_t now_tick);

/* Hard protection handlers */
static void update_hard_protection(const ChargeCycleConfig_t *cfg,
                                  const BMS_View_t *bms, uint32_t now_tick);

/* ============== Private Functions ============== */

static void set_fault(uint32_t flags, uint32_t now) {
    g_ctrl.fault_flags |= flags;
    if (flags & CHARGE_CTRL_FAULT_EMERGENCY_STOP) {
        g_ctrl.stop_reason = CHARGE_STOP_EMERGENCY;
    } else if (flags & CHARGE_CTRL_FAULT_BMS_OFFLINE) {
        g_ctrl.stop_reason = CHARGE_STOP_BMS_OFFLINE;
    } else if (flags & CHARGE_CTRL_FAULT_BMS_ALARM) {
        g_ctrl.stop_reason = CHARGE_STOP_BMS_ALARM;
    } else if (flags & (CHARGE_CTRL_FAULT_PROTECT_JACK_V |
                        CHARGE_CTRL_FAULT_PROTECT_JACK_TEMP)) {
        g_ctrl.stop_reason = CHARGE_STOP_PROTECTION;
    } else if (flags & CHARGE_CTRL_FAULT_MODULE_COUNT_MISMATCH) {
        g_ctrl.stop_reason = CHARGE_STOP_MODULE_MISMATCH;
    } else if (flags & (CHARGE_CTRL_FAULT_NO_MODULE |
                        CHARGE_CTRL_FAULT_NO_DRIVER)) {
        g_ctrl.stop_reason = CHARGE_STOP_PRECONDITION;
    }
    if (g_ctrl.state == CHARGE_CTRL_STATE_RUNNING) {
        transition_to(CHARGE_CTRL_STATE_FAULT, now);
    }
}

static void clear_fault(void) {
    g_ctrl.fault_flags = CHARGE_CTRL_FAULT_NONE;
}

static void transition_to(ChargeCtrlState_t new_state, uint32_t now) {
    if (g_ctrl.state == new_state) {
        return;
    }

    LOG("CC: State %d->%d\r\n", (int)g_ctrl.state, (int)new_state);
    g_ctrl.state = new_state;
    g_ctrl.last_update_tick = now;
}

static uint8_t get_active_module_count(void) {
    uint8_t count = 0;
    CHG_LIB_ModuleView_t view;

    uint8_t total = CHG_LIB_GetModuleCount();
    for (uint8_t i = 0; i < total; i++) {
        if (CHG_LIB_GetModuleView(i, &view)) {
            /* Count enabled modules that are not in fault/offline.
             * STOPPING is still considered active — module hasn't confirmed OFF yet. */
            if (view.enabled &&
                view.online &&
                view.state != CHG_LIB_STATE_OFFLINE &&
                view.state != CHG_LIB_STATE_FAULT) {
                count++;
            }
        }
    }
    return count;
}

/**
 * @brief Reference voltage for both the relay-arm 90% threshold and the
 *        Stage-1 (pre-relay-close) module setpoint -- see
 *        update_relay_decision() and the target_voltage_v assignment in
 *        run_bms_controlled_mode()/run_standalone_mode().
 * @note  BMS-Controlled mode: BmsView.batt_voltage (the pack's real,
 *        current terminal voltage, sensed by the BMS directly --
 *        independent of this relay's own state). Falls back to
 *        cfg->vmax_v if the BMS hasn't reported a usable voltage yet.
 *        Standalone (no-BMS) mode: always cfg->vmax_v -- no BMS reference
 *        exists in this mode by definition.
 */
static float compute_voltage_ref(const ChargeCycleConfig_t *cfg) {
    float voltage_ref = cfg->vmax_v;
    if (cfg->charge_source_mode == CHARGE_SOURCE_BMS_CONTROLLED) {
        BMS_View_t bms_view;
        BMS_GetView(&bms_view);
        if (bms_view.batt_voltage > 0.0f) {
            voltage_ref = bms_view.batt_voltage;
        }
    }
    return voltage_ref;
}

/**
 * @brief Decide whether the battery relay should be closed this tick.
 * @note  This is a latch, not a continuous gate: reaching the >=90% voltage
 *        threshold CLOSES the relay, but once closed it stays closed
 *        through normal voltage/current fluctuation (e.g. CV-phase current
 *        taper naturally sagging the bus below 90% again) -- only a real
 *        fault reopens it. "Fault" is:
 *          1. Controller state leaves RUNNING (FAULT/STOPPING/IDLE/READY) --
 *             applies in every charge_source_mode.
 *          2. Only when charge_source_mode is BMS-Controlled: the BMS
 *             itself stops confirming it's safe (BMS_ShouldCloseChargeRelay()
 *             goes false -- offline or critical alarm). Checked every
 *             tick even while latched closed, since a live BMS-reported
 *             fault mid-charge must open the relay immediately.
 *             BUGFIX 2026-08-29: BMS_ShouldCloseChargeRelay() used to
 *             also require the BMS's own self-reported internal
 *             charge-relay status (BmsSwSta charge_sta bit) to read
 *             closed -- removed, confirmed on real hardware that this
 *             BMS unit never closes its own relay in response to our
 *             Ctrl_INFO request, permanently blocking ours regardless of
 *             voltage; we do not actually control the BMS's own relay in
 *             this deployment (see bms_core.c). Standalone (no-BMS) mode
 *             never checks any of this: the system may have no BMS
 *             physically installed in that mode, so requiring
 *             BMS_IsOnline() would mean the relay could never close at
 *             all.
 *        Falling back below 90% target voltage is explicitly NOT a fault
 *        once latched -- by design, per product decision.
 *
 *        To (re-)arm the latch (only evaluated while not already latched
 *        closed): the worst-case (minimum) output voltage across all
 *        active (enabled, online, not OFFLINE/FAULT) modules must reach
 *        BMS_CHARGE_VOLT_LIMIT_PCT of a *reference* voltage -- i.e. every
 *        module must be up, not just one of several in a multi-module
 *        stack. The reference is:
 *          - BMS-Controlled mode: BmsView.batt_voltage (the pack's real,
 *            current terminal voltage, sensed by the BMS directly --
 *            independent of this relay's own state). BUGFIX 2026-08-29:
 *            this used to be target_voltage_v (the *final* charge
 *            setpoint) instead -- physically wrong for a pre-charge/
 *            inrush check (you want the charger's output close to the
 *            *battery's current* voltage before bridging them, not close
 *            to where the battery will end up after a full charge cycle;
 *            requiring 90% of a possibly much higher target needlessly
 *            overshoots the module before the relay ever closes) and,
 *            per real HIL testing, created a chicken-and-egg deadlock
 *            when the charger only has a load once this relay closes:
 *            some modules refuse to ramp output voltage at all while
 *            genuinely unloaded, so 90%-of-target could never be reached
 *            in the first place. batt_voltage is already known safe to
 *            use here -- BMS_ShouldCloseChargeRelay() above already
 *            required the BMS online with fresh data.
 *          - Standalone (no-BMS) mode: still target_voltage_v -- no BMS
 *            reference exists in this mode by definition.
 *
 *        The latch resets to "not yet armed" the instant state leaves
 *        RUNNING, so the next RUNNING session must earn >=90% again from
 *        scratch.
 *
 *        Opening is NOT immediate once latched closed (2026-08-29,
 *        user-confirmed): breaking a DC relay while real charging current
 *        is still flowing risks arcing/contact welding (DC has no natural
 *        current zero-crossing the way AC does). When a fault/stop
 *        condition is seen while latched closed, relay_should_close stays
 *        true (module stop is already commanded elsewhere -- see
 *        run_bms_controlled_mode()'s critical-alarm check and the
 *        STOPPING/FAULT state handlers' stop_charging() calls, both of
 *        which fire independently of this function) until either the
 *        worst-case (max) module current reads below
 *        RELAY_OPEN_CURRENT_THRESHOLD_A, or RELAY_OPEN_TIMEOUT_MS elapses
 *        (bounds the wait if a module goes silent mid-ramp-down instead of
 *        genuinely reaching zero). EMERGENCY_STOP is the one exception --
 *        it opens immediately regardless of current, per explicit product
 *        decision: at that point speed matters more than the arc risk.
 *        If the relay was never latched closed this RUNNING session, there
 *        was never a load to begin with, so it opens immediately too --
 *        nothing to wait for.
 */
static void update_relay_decision(uint32_t now_tick) {
    bool controller_wants_relay = (g_ctrl.state == CHARGE_CTRL_STATE_RUNNING);

    ChargeCycleConfig_t cfg;
    ChargeCycleConfig_Get(&cfg);
    bool bms_safe = true;
    if (controller_wants_relay && cfg.charge_source_mode == CHARGE_SOURCE_BMS_CONTROLLED) {
        bms_safe = BMS_ShouldCloseChargeRelay();
    }

    bool fault_condition = !controller_wants_relay || !bms_safe;
    bool is_emergency = (g_ctrl.fault_flags & CHARGE_CTRL_FAULT_EMERGENCY_STOP) != 0U;

    if (fault_condition) {
        if (!g_ctrl.relay_latched_closed) {
            /* Never armed this session -- no load was ever bridged, open
             * immediately, nothing to wait for. */
            g_ctrl.relay_should_close = false;
            g_ctrl.relay_open_pending = false;
            return;
        }

        if (is_emergency) {
            /* User-confirmed: emergency overrides the current-settle wait. */
            g_ctrl.relay_latched_closed = false;
            g_ctrl.relay_should_close = false;
            g_ctrl.relay_open_pending = false;
            return;
        }

        if (!g_ctrl.relay_open_pending) {
            g_ctrl.relay_open_pending = true;
            g_ctrl.relay_open_pending_tick = now_tick;
        }

        float max_current = -1.0f;
        CHG_LIB_ModuleView_t cur_view;
        uint8_t total = CHG_LIB_GetModuleCount();
        for (uint8_t i = 0; i < total; i++) {
            if (!CHG_LIB_GetModuleView(i, &cur_view)) continue;
            if (!cur_view.enabled) continue;
            /* No online/state filter here (unlike the arm-side voltage
             * loop) -- a module that goes silent mid-ramp-down should not
             * be assumed to have reached 0A just because comms dropped;
             * RELAY_OPEN_TIMEOUT_MS is what bounds that case instead. */
            if (cur_view.current > max_current) {
                max_current = cur_view.current;
            }
        }

        bool current_settled = (max_current >= 0.0f && max_current < RELAY_OPEN_CURRENT_THRESHOLD_A);
        bool timed_out = (now_tick - g_ctrl.relay_open_pending_tick) >= RELAY_OPEN_TIMEOUT_MS;

        if (current_settled || timed_out) {
            if (timed_out && !current_settled) {
                int i_int = (int)(max_current * 10.0f);
                LOG("CC: relay open timeout, current still %d.%dA\r\n", i_int / 10, i_int % 10);
            }
            g_ctrl.relay_latched_closed = false;
            g_ctrl.relay_should_close = false;
            g_ctrl.relay_open_pending = false;
        } else {
            g_ctrl.relay_should_close = true; /* keep closed while draining down */
        }
        return;
    }

    g_ctrl.relay_open_pending = false;

    if (g_ctrl.relay_latched_closed) {
        /* Already earned it this session: stay closed regardless of
         * voltage now (see docstring) -- RUNNING + BMS-safe (both already
         * checked above) is all that's still required. */
        g_ctrl.relay_should_close = true;
        return;
    }

    /* Not yet latched: check whether this tick arms it. */
    g_ctrl.relay_should_close = false;
    if (g_ctrl.target_voltage_v <= 0.0f) {
        return;
    }

    float voltage_ref = compute_voltage_ref(&cfg);

    float min_voltage = -1.0f;
    CHG_LIB_ModuleView_t view;
    uint8_t total = CHG_LIB_GetModuleCount();
    for (uint8_t i = 0; i < total; i++) {
        if (!CHG_LIB_GetModuleView(i, &view)) continue;
        if (!view.enabled || !view.online) continue;
        if (view.state == CHG_LIB_STATE_OFFLINE || view.state == CHG_LIB_STATE_FAULT) continue;
        if (min_voltage < 0.0f || view.voltage < min_voltage) {
            min_voltage = view.voltage;
        }
    }
    if (min_voltage < 0.0f) {
        return; /* no active module reporting voltage */
    }
    if (min_voltage < (voltage_ref * ((float)BMS_CHARGE_VOLT_LIMIT_PCT / 100.0f))) {
        return;
    }

    g_ctrl.relay_latched_closed = true;
    g_ctrl.relay_should_close = true;
}

/**
 * @brief Tell the BMS whether we want to charge (Ctrl_INFO chg_sw), so its
 *        own internal charge relay can close.
 * @note  BUGFIX 2026-08-29: BMS_SendCtrlInfo() (Modules/bms/bms_core.c)
 *        existed to set this but had no caller anywhere in the firmware --
 *        g_charge_ctrl.allow_charge stayed false forever (zero-init,
 *        never written), so the periodic 500ms Ctrl_INFO transmission
 *        (FR-BMS-06, transmit_ctrl_info() in bms_core.c) kept firing on
 *        schedule but with chg_sw always 0, even while actively RUNNING.
 *        A BMS that gates its own internal charge relay on this signal
 *        would then never report charge_relay_closed=true, so
 *        update_relay_decision()'s BMS_ShouldCloseChargeRelay() check
 *        would block the MCU's own relay forever -- independent of, and
 *        regardless of, module output voltage. Confirmed against a real
 *        HIL run: module voltage already >90% of target, relay still
 *        never closed.
 *        allow_charge=true only while actually RUNNING (not READY/
 *        STOPPING/FAULT) -- user-confirmed choice, the safest reading of
 *        "we are in a real charge cycle right now". Edge-triggered (only
 *        calls BMS_SendCtrlInfo() when the desired value changes) so this
 *        doesn't add an extra 20ms-tick CAN TX on top of Ctrl_INFO's
 *        existing periodic cadence -- same reasoning as BUG-05's fix to
 *        apply_charge_targets(). */
static void update_bms_charge_allow(void) {
    bool want_allow = (g_ctrl.state == CHARGE_CTRL_STATE_RUNNING);
    if (want_allow != g_ctrl.bms_charge_allow_sent) {
        g_ctrl.bms_charge_allow_sent = want_allow;
        BMS_ChargeCtrl_t ctrl = { .allow_charge = want_allow, .allow_discharge = false };
        BMS_SendCtrlInfo(&ctrl);
    }
}

/**
 * @brief Check preconditions without modifying state or setting faults
 * @return fault flags if any check fails, 0 if all pass
 */
static uint32_t check_preconditions_faults(void) {
    uint32_t faults = CHARGE_CTRL_FAULT_NONE;
    ChargeCycleConfig_t cfg;
    uint8_t actual_count;

    /* Check driver registered */
    if (CHG_LIB_GetActiveDriverId() == CHG_LIB_DRV_NONE) {
        LOG("ChargeController_Check: No driver registered\r\n");
        faults |= CHARGE_CTRL_FAULT_NO_DRIVER;
    }

    /* Check module count */
    actual_count = get_active_module_count();

    if (actual_count == 0) {
        LOG("ChargeController_Check: No active modules\r\n");
        faults |= CHARGE_CTRL_FAULT_NO_MODULE;
    } else {
        ChargeCycleConfig_Get(&cfg);
        if (actual_count != cfg.source_module_count) {
            LOG("ChargeController_Check: Module count mismatch: expected %u, got %u\r\n",
                (unsigned)cfg.source_module_count, (unsigned)actual_count);
            faults |= CHARGE_CTRL_FAULT_MODULE_COUNT_MISMATCH;
        }
    }

    /* Check config validity */
    ChargeCycleConfig_Get(&cfg);
    if (cfg.version != CHARGE_CYCLE_CONFIG_VERSION) {
        LOG("ChargeController_Check: Invalid config version %u\r\n", (unsigned)cfg.version);
        faults |= CHARGE_CTRL_FAULT_INVALID_CONFIG;
    }

    return faults;
}

/**
 * @brief Check preconditions and set faults (internal use)
 */
static bool check_preconditions_set_fault(uint32_t now) {
    uint32_t faults = check_preconditions_faults();
    if (faults != CHARGE_CTRL_FAULT_NONE) {
        set_fault(faults, now);
        return false;
    }

    /* Store actual counts */
    ChargeCycleConfig_t cfg;
    ChargeCycleConfig_Get(&cfg);
    g_ctrl.source_module_count = cfg.source_module_count;
    g_ctrl.actual_module_count = get_active_module_count();

    return true;
}

static void apply_charge_targets(void) {
    bool should_run = (g_ctrl.target_current_total_a > 0.0f && !g_ctrl.inhibit);

    if (should_run && !g_ctrl.last_running) {
        /* Force apply on start */
        CHG_LIB_SetVoltageAll(g_ctrl.target_voltage_v);
        CHG_LIB_SetCurrentLimitAll(g_ctrl.target_current_per_module_a);
        CHG_LIB_StartAll();
        
        int v_int = (int)(g_ctrl.target_voltage_v * 10.0f);
        int i_int = (int)(g_ctrl.target_current_per_module_a * 10.0f);
        LOG("CC: Start V=%d.%dV I=%d.%dA/mod\r\n",
            v_int / 10, v_int % 10, i_int / 10, i_int % 10);
    } else if (should_run) {
        /* Only send if changed */
        if (g_ctrl.target_voltage_v != g_ctrl.applied_voltage_v) {
            CHG_LIB_SetVoltageAll(g_ctrl.target_voltage_v);
        }
        if (g_ctrl.target_current_per_module_a != g_ctrl.applied_current_per_module_a) {
            CHG_LIB_SetCurrentLimitAll(g_ctrl.target_current_per_module_a);
        }
    } else if (!should_run && g_ctrl.last_running) {
        CHG_LIB_StopAll();
        if (g_ctrl.inhibit) {
            g_ctrl.stop_reason = CHARGE_STOP_STAGE_INHIBIT;
        }
        LOG("CC: Stop inhibit=%u derating=%u\r\n",
            g_ctrl.inhibit, g_ctrl.derating);
    }

    g_ctrl.applied_voltage_v = g_ctrl.target_voltage_v;
    g_ctrl.applied_current_per_module_a = g_ctrl.target_current_per_module_a;
    g_ctrl.last_running = should_run;
}

static void stop_charging(void) {
    CHG_LIB_StopAll();
    g_ctrl.target_voltage_v = 0.0f;
    g_ctrl.target_current_total_a = 0.0f;
    g_ctrl.target_current_per_module_a = 0.0f;
    g_ctrl.applied_voltage_v = 0.0f;
    g_ctrl.applied_current_per_module_a = 0.0f;
    g_ctrl.standalone_vmax_reached_tick = 0;
    g_ctrl.last_running = 0;
}

/**
 * @brief Complete a standalone charge when any fresh module reaches Vmax.
 *
 * No-BMS mode has no battery-voltage feedback. The only usable completion
 * signal is the actual output voltage reported by a charger module. A single
 * valid module reaching the effective target is sufficient to stop all
 * modules; the reading must remain valid for a short confirmation window.
 */
static bool check_standalone_voltage_reached(uint32_t now_tick) {
    if (g_ctrl.target_voltage_v <= 0.0f) {
        g_ctrl.standalone_vmax_reached_tick = 0;
        return false;
    }

    CHG_LIB_ModuleView_t view;
    uint8_t module_count = CHG_LIB_GetModuleCount();
    bool reached = false;

    for (uint8_t i = 0; i < module_count; i++) {
        if (!CHG_LIB_GetModuleView(i, &view)) {
            continue;
        }

        /* Ignore disabled, faulted, offline, or stale telemetry. */
        if (!view.enabled || !view.online ||
            view.state == CHG_LIB_STATE_OFFLINE ||
            view.state == CHG_LIB_STATE_FAULT ||
            view.last_rx_tick == 0U ||
            (now_tick - view.last_rx_tick) > CHARGE_CTRL_MODULE_VOLTAGE_MAX_AGE_MS ||
            view.voltage <= 0.0f) {
            continue;
        }

        if (view.voltage >= g_ctrl.target_voltage_v) {
            reached = true;
            break;
        }
    }

    if (!reached) {
        g_ctrl.standalone_vmax_reached_tick = 0;
        return false;
    }

    if (g_ctrl.standalone_vmax_reached_tick == 0U) {
        g_ctrl.standalone_vmax_reached_tick = now_tick;
        int target_x10 = (int)(g_ctrl.target_voltage_v * 10.0f);
        LOG("CC: Standalone Vmax reached candidate (target=%d.%dV)\r\n",
            target_x10 / 10, target_x10 % 10);
        return false;
    }

    if ((now_tick - g_ctrl.standalone_vmax_reached_tick) <
        CHARGE_CTRL_STANDALONE_VMAX_CONFIRM_MS) {
        return false;
    }

    g_ctrl.stop_reason = CHARGE_STOP_VOLTAGE_REACHED;
    int target_x10 = (int)(g_ctrl.target_voltage_v * 10.0f);
    LOG("CC: Standalone Vmax reached, stopping charge (target=%d.%dV)\r\n",
        target_x10 / 10, target_x10 % 10);
    transition_to(CHARGE_CTRL_STATE_STOPPING, now_tick);
    return true;
}

/* ============== Hard Protection ============== */

/**
 * @brief Update hard protection timers and trigger faults if conditions persist
 *
 * Hard protection differs from stage inhibit:
 * - Stage inhibit: current = 0, but not necessarily a fault
 * - Hard protection: triggers FAULT if condition persists > delay_s
 */
static void update_hard_protection(const ChargeCycleConfig_t *cfg,
                                  const BMS_View_t *bms, uint32_t now_tick) {
    bool jack_v_protect_active = false;

    /* Only apply battery-dependent protections if BMS is online */
    if (bms->online) {
        /* ----- Jack voltage protection ----- */
        if (cfg->protect_jack_charge_enabled) {
            CHG_LIB_SystemSummary_t sys_summary;
            CHG_LIB_GetSystemSummary(&sys_summary);
            float delta_v = sys_summary.voltage - bms->batt_voltage;
            if (delta_v > cfg->protect_jack_charge_delta_v) {
                jack_v_protect_active = true;
            }
        }
    }

    /* Jack temperature protection logic is now handled as soft derating in run_bms_controlled_mode */

    /* ----- Update timers and check delays ----- */

    /* Jack voltage protection */
    if (jack_v_protect_active) {
        if (g_ctrl.protect_jack_v_timer_tick == 0) {
            g_ctrl.protect_jack_v_timer_tick = now_tick;
            int thresh_x10 = (int)(cfg->protect_jack_charge_delta_v * 10.0f);
            LOG("CC: Jack V protect started (delta=%d.%dV)\r\n", thresh_x10 / 10, thresh_x10 % 10);
        } else {
            uint32_t elapsed_s = (now_tick - g_ctrl.protect_jack_v_timer_tick) / 1000U;
            if (elapsed_s >= cfg->protect_jack_charge_delay_s) {
                LOG("CC: Jack V PROTECT fault (%us)\r\n", (unsigned)elapsed_s);
                set_fault(CHARGE_CTRL_FAULT_PROTECT_JACK_V, now_tick);
            }
        }
    } else {
        g_ctrl.protect_jack_v_timer_tick = 0;  /* Reset timer */
    }
}

/* ============== Stage Evaluation ============== */

/* ============== Stage Evaluation Helpers ============== */

static float get_lower_threshold_for_band(const ChargeCycleConfig_t *cfg, ChargeLimitSource_t source, ChargeStageBand_t band) {
    if (source == CHARGE_LIMIT_SOURCE_CELL_VOLTAGE) {
        switch (band) {
            case CHARGE_STAGE_BAND_1_2: return cfg->cell_volt_1_v;
            case CHARGE_STAGE_BAND_2_3: return cfg->cell_volt_2_v;
            case CHARGE_STAGE_BAND_3_4: return cfg->cell_volt_3_v;
            case CHARGE_STAGE_BAND_4_5: return cfg->cell_volt_4_v;
            case CHARGE_STAGE_BAND_ABOVE_MAX: return cfg->cell_volt_5_v;
            default: return 0.0f;
        }
    } else if (source == CHARGE_LIMIT_SOURCE_TEMPERATURE) {
        switch (band) {
            case CHARGE_STAGE_BAND_1_2: return cfg->temp_1_c;
            case CHARGE_STAGE_BAND_2_3: return cfg->temp_2_c;
            case CHARGE_STAGE_BAND_3_4: return cfg->temp_3_c;
            case CHARGE_STAGE_BAND_4_5: return cfg->temp_4_c;
            case CHARGE_STAGE_BAND_ABOVE_MAX: return cfg->temp_5_c;
            default: return -100.0f;
        }
    } else if (source == CHARGE_LIMIT_SOURCE_SOC) {
        switch (band) {
            case CHARGE_STAGE_BAND_1_2: return cfg->soc_1_pct;
            case CHARGE_STAGE_BAND_2_3: return cfg->soc_2_pct;
            case CHARGE_STAGE_BAND_3_4: return cfg->soc_3_pct;
            case CHARGE_STAGE_BAND_4_5: return cfg->soc_4_pct;
            case CHARGE_STAGE_BAND_ABOVE_MAX: return cfg->soc_5_pct;
            default: return 0.0f;
        }
    }
    return 0.0f;
}

/**
 * @brief Shared 5-threshold band lookup, identical across all 3 stage
 *        sources (cell voltage / temperature / SOC) -- see the callers
 *        below. Only this ladder is truly identical between the three;
 *        what happens next (monotonic latch for cell/SOC vs. symmetric
 *        hysteresis for temp) genuinely differs per source and is kept
 *        inline in each eval_*_stage(), not forced into a shared helper.
 */
static ChargeStageBand_t band_from_thresholds(float value, float t1, float t2,
                                              float t3, float t4, float t5) {
    if (value >= t5) return CHARGE_STAGE_BAND_ABOVE_MAX;
    if (value >= t4) return CHARGE_STAGE_BAND_4_5;
    if (value >= t3) return CHARGE_STAGE_BAND_3_4;
    if (value >= t2) return CHARGE_STAGE_BAND_2_3;
    if (value >= t1) return CHARGE_STAGE_BAND_1_2;
    return CHARGE_STAGE_BAND_BELOW_MIN;
}

/**
 * @brief Shared band->current-limit mapping, identical shape across all 3
 *        stage sources -- only which per-band current constants get read
 *        differs (curr1..curr4, one per band, source-specific).
 *
 * @note  ABOVE_MAX always sets eval->inhibit=1 here (current_limit_c=0), but
 *        that flag means two DIFFERENT things depending on which caller you
 *        are, because the underlying business rule differs per stage --
 *        confirmed with the user 2026-08-29: within one charge cycle, cell
 *        voltage and SOC only ever move forward (band N -> N+1, never back,
 *        even if the raw reading dips); temperature is the only one of the
 *        three allowed to step back down a band, and only once it drops
 *        below (lower_threshold - temp_delta_c) (see eval_temp_stage's
 *        hysteresis).
 *          - eval_temp_stage(): ABOVE_MAX is a genuine LIVE, RECOVERABLE
 *            inhibit (FR-CTRL-11) -- current goes to 0 while over-temp, and
 *            resumes on its own once temperature drops back into range.
 *            Callers may use eval.inhibit directly for this source.
 *          - eval_cell_stage()/eval_soc_stage(): ABOVE_MAX is a COMPLETION
 *            latch (FR-CTRL-09/10), not a recoverable inhibit -- the charge
 *            cycle is meant to END (STOPPING), not just pause. These two
 *            functions track that separately via g_ctrl.cell_full_latched /
 *            g_ctrl.soc_full_latched (set as a side effect, band forced to
 *            stay ABOVE_MAX once latched). Callers MUST check those latch
 *            flags for completion BEFORE treating eval.inhibit as an
 *            ordinary current-gating inhibit for these two sources -- see
 *            run_bms_controlled_mode()'s completion check, which runs before
 *            the inhibit-driven current calculation for exactly this reason.
 */
static void apply_band_current_limit(ChargeStageBand_t band, float curr1, float curr2,
                                     float curr3, float curr4, ChargeStageEval_t *eval) {
    eval->band = band;
    switch (band) {
        case CHARGE_STAGE_BAND_BELOW_MIN:
            eval->state = CHARGE_STAGE_BELOW_MIN;
            eval->inhibit = 1;
            eval->current_limit_c = 0.0f;
            break;
        case CHARGE_STAGE_BAND_1_2:
            eval->state = CHARGE_STAGE_IN_WINDOW;
            eval->current_limit_c = curr1;
            break;
        case CHARGE_STAGE_BAND_2_3:
            eval->state = CHARGE_STAGE_IN_WINDOW;
            eval->current_limit_c = curr2;
            break;
        case CHARGE_STAGE_BAND_3_4:
            eval->state = CHARGE_STAGE_IN_WINDOW;
            eval->current_limit_c = curr3;
            break;
        case CHARGE_STAGE_BAND_4_5:
            eval->state = CHARGE_STAGE_IN_WINDOW;
            eval->current_limit_c = curr4;
            break;
        default:
            eval->state = CHARGE_STAGE_ABOVE_MAX;
            eval->band = CHARGE_STAGE_BAND_ABOVE_MAX;
            eval->inhibit = 1;
            eval->current_limit_c = 0.0f;
            break;
    }
}

static ChargeStageEval_t eval_cell_stage(const ChargeCycleConfig_t *cfg, const BMS_View_t *bms) {
    ChargeStageEval_t eval = {0};

    if (!cfg->cell_volt_enabled) {
        eval.enabled = 0;
        return eval;
    }

    eval.enabled = 1;
    eval.source = CHARGE_LIMIT_SOURCE_CELL_VOLTAGE;
    float cell_volt_v = (float)bms->max_cell_volt / 1000.0f;  /* mV -> V */

    ChargeStageBand_t new_band = band_from_thresholds(
        cell_volt_v, cfg->cell_volt_1_v, cfg->cell_volt_2_v,
        cfg->cell_volt_3_v, cfg->cell_volt_4_v, cfg->cell_volt_5_v);

    /* Cell voltage is monotonic within one user-started cycle.  A drop
     * below a previously reached band must never restore a higher current.
     * Below-min remains a live block, but does not erase progress. */
    if (new_band == CHARGE_STAGE_BAND_ABOVE_MAX) {
        g_ctrl.cell_full_latched = true;
        g_ctrl.max_cell_band = CHARGE_STAGE_BAND_ABOVE_MAX;
    } else if (new_band == CHARGE_STAGE_BAND_BELOW_MIN) {
        /* Keep the high-watermark unchanged while blocking current. */
    } else if (new_band > g_ctrl.max_cell_band) {
        g_ctrl.max_cell_band = new_band;
    }

    g_ctrl.last_cell_band = new_band;
    if (g_ctrl.cell_full_latched) {
        new_band = CHARGE_STAGE_BAND_ABOVE_MAX;
    } else if (new_band != CHARGE_STAGE_BAND_BELOW_MIN &&
               g_ctrl.max_cell_band > new_band) {
        new_band = g_ctrl.max_cell_band;
    }

    apply_band_current_limit(new_band, cfg->cell_curr_1_c, cfg->cell_curr_2_c,
                             cfg->cell_curr_3_c, cfg->cell_curr_4_c, &eval);
    return eval;
}

static ChargeStageEval_t eval_temp_stage(const ChargeCycleConfig_t *cfg, const BMS_View_t *bms) {
    ChargeStageEval_t eval = {0};

    if (!cfg->temp_enabled) {
        eval.enabled = 0;
        return eval;
    }

    eval.enabled = 1;
    eval.source = CHARGE_LIMIT_SOURCE_TEMPERATURE;
    float temp_c = (float)bms->max_cell_temp;  /* Already in Celsius */

    ChargeStageBand_t new_band = band_from_thresholds(
        temp_c, cfg->temp_1_c, cfg->temp_2_c, cfg->temp_3_c, cfg->temp_4_c, cfg->temp_5_c);

    /* Apply hysteresis */
    if (g_ctrl.last_temp_band != CHARGE_STAGE_BAND_NONE) {
        if (new_band < g_ctrl.last_temp_band) {
            float lower_thresh = get_lower_threshold_for_band(cfg, CHARGE_LIMIT_SOURCE_TEMPERATURE, g_ctrl.last_temp_band);
            if (temp_c >= (lower_thresh - cfg->temp_delta_c)) {
                new_band = g_ctrl.last_temp_band; /* Keep current band */
            }
        } else if (new_band > g_ctrl.last_temp_band) {
            float upper_thresh = get_lower_threshold_for_band(cfg, CHARGE_LIMIT_SOURCE_TEMPERATURE, new_band);
            if (temp_c <= (upper_thresh + cfg->temp_delta_c)) {
                new_band = g_ctrl.last_temp_band; /* Keep current band */
            }
        }
    }

    g_ctrl.last_temp_band = new_band;

    apply_band_current_limit(new_band, cfg->temp_curr_1_c, cfg->temp_curr_2_c,
                             cfg->temp_curr_3_c, cfg->temp_curr_4_c, &eval);
    return eval;
}

static ChargeStageEval_t eval_soc_stage(const ChargeCycleConfig_t *cfg, const BMS_View_t *bms) {
    ChargeStageEval_t eval = {0};

    if (!cfg->soc_enabled) {
        eval.enabled = 0;
        return eval;
    }

    eval.enabled = 1;
    eval.source = CHARGE_LIMIT_SOURCE_SOC;
    float soc_pct = (float)bms->soc;  /* Already in percentage */

    ChargeStageBand_t new_band = band_from_thresholds(
        soc_pct, cfg->soc_1_pct, cfg->soc_2_pct, cfg->soc_3_pct, cfg->soc_4_pct, cfg->soc_5_pct);

    /* SOC is monotonic within one user-started cycle.  A decrease never
     * lowers the charge level or increases the current again. */
    if (new_band == CHARGE_STAGE_BAND_ABOVE_MAX) {
        g_ctrl.soc_full_latched = true;
        g_ctrl.max_soc_band = CHARGE_STAGE_BAND_ABOVE_MAX;
    } else if (new_band == CHARGE_STAGE_BAND_BELOW_MIN) {
        /* Keep the high-watermark unchanged while blocking current. */
    } else if (new_band > g_ctrl.max_soc_band) {
        g_ctrl.max_soc_band = new_band;
    }

    g_ctrl.last_soc_band = new_band;
    if (g_ctrl.soc_full_latched) {
        new_band = CHARGE_STAGE_BAND_ABOVE_MAX;
    } else if (new_band != CHARGE_STAGE_BAND_BELOW_MIN &&
               g_ctrl.max_soc_band > new_band) {
        new_band = g_ctrl.max_soc_band;
    }

    apply_band_current_limit(new_band, cfg->soc_curr_1_c, cfg->soc_curr_2_c,
                             cfg->soc_curr_3_c, cfg->soc_curr_4_c, &eval);
    return eval;
}

static bool compute_stage_limits(const ChargeCycleConfig_t *cfg, const BMS_View_t *bms,
                                 float *limit_c_out, uint8_t *inhibit_out,
                                 uint8_t *limit_source_out, uint8_t *stage_band_out) {
    ChargeStageEval_t cell_eval = eval_cell_stage(cfg, bms);
    ChargeStageEval_t temp_eval = eval_temp_stage(cfg, bms);
    ChargeStageEval_t soc_eval = eval_soc_stage(cfg, bms);

    /* Count enabled groups */
    uint8_t enabled_count = cell_eval.enabled + temp_eval.enabled + soc_eval.enabled;

    if (enabled_count == 0) {
        /* No stage limits - use imax_c as default */
        *limit_c_out = cfg->imax_c;
        *inhibit_out = 0;
        *limit_source_out = CHARGE_LIMIT_SOURCE_NONE;
        *stage_band_out = CHARGE_STAGE_BAND_NONE;
        return true;
    }

    /* Check if any group is inhibited */
    if (cell_eval.inhibit || temp_eval.inhibit || soc_eval.inhibit) {
        *limit_c_out = 0.0f;
        *inhibit_out = 1;
        if (cell_eval.inhibit) {
            *limit_source_out = cell_eval.source;
            *stage_band_out = cell_eval.band;
        } else if (temp_eval.inhibit) {
            *limit_source_out = temp_eval.source;
            *stage_band_out = temp_eval.band;
        } else {
            *limit_source_out = soc_eval.source;
            *stage_band_out = soc_eval.band;
        }
        return true;
    }

    /* Take minimum of all enabled groups */
    float min_limit = 1e9f;  /* Large number */
    uint8_t min_source = CHARGE_LIMIT_SOURCE_NONE;
    uint8_t min_band = CHARGE_STAGE_BAND_NONE;

    if (cell_eval.enabled && cell_eval.current_limit_c <= min_limit) {
        min_limit = cell_eval.current_limit_c;
        min_source = cell_eval.source;
        min_band = cell_eval.band;
    }
    if (temp_eval.enabled && temp_eval.current_limit_c < min_limit) {
        min_limit = temp_eval.current_limit_c;
        min_source = temp_eval.source;
        min_band = temp_eval.band;
    }
    if (soc_eval.enabled && soc_eval.current_limit_c < min_limit) {
        min_limit = soc_eval.current_limit_c;
        min_source = soc_eval.source;
        min_band = soc_eval.band;
    }

    *limit_c_out = min_limit;
    *inhibit_out = 0;
    *limit_source_out = min_source;
    *stage_band_out = min_band;
    return true;
}

/* ============== Mode Handlers Helpers ============== */

static void apply_jack_temp_derating(const ChargeCycleConfig_t *cfg, uint32_t now_tick) {
    /* ----- Jack temperature soft derating -----
     * mcu_adc_temp_c is the max of the 4 NTC channels, read by the
     * composition root and pushed in via ChargeController_SetJackTempC()
     * every control cycle (see AGENTS.md sec 5-6: no direct BSP access from
     * pure charging policy). */
    float mcu_adc_temp_c = g_ctrl.jack_temp_input_c;

    if (cfg->protect_jack_temp_enabled) {
        if (mcu_adc_temp_c >= cfg->protect_jack_temp_threshold_c) {
            if (!g_ctrl.jack_temp_derating_active) {
                if (g_ctrl.protect_jack_temp_timer_tick == 0) {
                    g_ctrl.protect_jack_temp_timer_tick = now_tick;
                } else {
                    uint32_t elapsed_s = (now_tick - g_ctrl.protect_jack_temp_timer_tick) / 1000U;
                    if (elapsed_s >= cfg->protect_jack_temp_delay_s) {
                        g_ctrl.jack_temp_derating_active = true;
                        g_ctrl.protect_jack_temp_timer_tick = 0;
                        LOG("CC: Jack temp derating active\r\n");
                    }
                }
            } else {
                g_ctrl.protect_jack_temp_timer_tick = 0;
            }
        } else if (mcu_adc_temp_c <= (cfg->protect_jack_temp_threshold_c - cfg->protect_jack_temp_delta_c)) {
            if (g_ctrl.jack_temp_derating_active) {
                if (g_ctrl.protect_jack_temp_timer_tick == 0) {
                    g_ctrl.protect_jack_temp_timer_tick = now_tick;
                } else {
                    uint32_t elapsed_s = (now_tick - g_ctrl.protect_jack_temp_timer_tick) / 1000U;
                    if (elapsed_s >= cfg->protect_jack_temp_delay_s) {
                        g_ctrl.jack_temp_derating_active = false;
                        g_ctrl.protect_jack_temp_timer_tick = 0;
                        LOG("CC: Jack temp derating recovered\r\n");
                    }
                }
            } else {
                g_ctrl.protect_jack_temp_timer_tick = 0;
            }
        } else {
            g_ctrl.protect_jack_temp_timer_tick = 0;
        }
    } else {
        g_ctrl.jack_temp_derating_active = false;
        g_ctrl.protect_jack_temp_timer_tick = 0;
    }

    if (g_ctrl.jack_temp_derating_active) {
        float limit_pct = cfg->protect_jack_temp_power_limit_pct;
        if (limit_pct < 0.0f) limit_pct = 0.0f;
        if (limit_pct > 100.0f) limit_pct = 100.0f;
        g_ctrl.target_current_total_a *= (limit_pct / 100.0f);
        g_ctrl.derating = 1;
    }
}

/* ============== Mode Handlers ============== */

/**
 * @brief Manual mode: hold the operator-supplied V/I setpoint fixed for the
 *        whole RUNNING session.
 * @note  Confirmed with the user 2026-08-29: Manual is meant to run at one
 *        constant setpoint for its entire duration -- no stage bands
 *        (cell/temp/SOC), no jack-temp soft derating (apply_jack_temp_derating()
 *        is intentionally NOT called here, unlike run_standalone_mode()/
 *        run_bms_controlled_mode()). The operator who set Manual owns the
 *        setpoint and any thermal/charge-curve judgment behind it. Hard
 *        protection (jack-V fault, module alarms, E-STOP) still applies --
 *        that runs in ChargeController_Process() before this handler is
 *        even called, unconditionally of mode -- only the *soft*, automatic
 *        derating/staging is skipped.
 */
static void run_manual_mode(uint32_t now_tick) {
    (void)now_tick;

    ChargeCycleConfig_t cfg;
    ChargeCycleConfig_Get(&cfg);

    g_ctrl.target_voltage_v = g_ctrl.manual_target_voltage_v;
    g_ctrl.target_current_per_module_a = g_ctrl.manual_target_current_per_module_a;
    g_ctrl.target_current_total_a = g_ctrl.target_current_per_module_a * (float)g_ctrl.actual_module_count;

    g_ctrl.inhibit = 0;
    g_ctrl.derating = 0;
    g_ctrl.active_limit_source = CHARGE_LIMIT_SOURCE_NONE;
    g_ctrl.active_stage_band = CHARGE_STAGE_BAND_NONE;
    g_ctrl.active_limit_current_c = 0.0f;

    /* Clamp to hardware limits */
    if (g_ctrl.target_voltage_v > cfg.module_u_max_v) {
        g_ctrl.target_voltage_v = cfg.module_u_max_v;
    }
    if (g_ctrl.target_current_per_module_a > cfg.module_i_max_a) {
        g_ctrl.target_current_per_module_a = cfg.module_i_max_a;
        g_ctrl.target_current_total_a = g_ctrl.target_current_per_module_a * (float)g_ctrl.actual_module_count;
    }

    int v_int = (int)(g_ctrl.target_voltage_v * 10.0f);
    int i_int = (int)(g_ctrl.target_current_per_module_a * 10.0f);
    static int last_v_int = -1, last_i_int = -1;
    if (v_int != last_v_int || i_int != last_i_int) {
        last_v_int = v_int;
        last_i_int = i_int;
        LOG("CC: Manual V=%d.%dV I=%d.%dA/mod\r\n",
            v_int / 10, v_int % 10, i_int / 10, i_int % 10);
    }

    apply_charge_targets();
}

static void run_standalone_mode(uint32_t now_tick) {
    (void)now_tick;

    ChargeCycleConfig_t cfg;
    ChargeCycleConfig_Get(&cfg);

    /* Set targets from config Pack Parameters */
    g_ctrl.target_voltage_v = cfg.vmax_v;
    g_ctrl.target_current_total_a = cfg.imax_c * cfg.battery_capacity_ah;

    g_ctrl.inhibit = 0;
    g_ctrl.derating = 0;
    g_ctrl.active_limit_source = CHARGE_LIMIT_SOURCE_NONE;
    g_ctrl.active_stage_band = CHARGE_STAGE_BAND_NONE;
    g_ctrl.active_limit_current_c = 0.0f;

    /* Apply Jack Temp Derating */
    apply_jack_temp_derating(&cfg, now_tick);

    /* Per-module split */
    if (g_ctrl.actual_module_count > 0) {
        g_ctrl.target_current_per_module_a = g_ctrl.target_current_total_a / (float)g_ctrl.actual_module_count;
    } else {
        g_ctrl.target_current_per_module_a = 0.0f;
    }

    /* Clamp to hardware limits */
    if (g_ctrl.target_voltage_v > cfg.module_u_max_v) {
        g_ctrl.target_voltage_v = cfg.module_u_max_v;
    }
    if (g_ctrl.target_current_per_module_a > cfg.module_i_max_a) {
        g_ctrl.target_current_per_module_a = cfg.module_i_max_a;
        /* Recalculate total current */
        g_ctrl.target_current_total_a = g_ctrl.target_current_per_module_a * (float)g_ctrl.actual_module_count;
        g_ctrl.derating = 1;
    }

    /* No-BMS completion: one fresh module at effective Vmax stops all. */
    if (check_standalone_voltage_reached(now_tick)) {
        return;
    }

    int v_int = (int)(g_ctrl.target_voltage_v * 10.0f);
    int i_int = (int)(g_ctrl.target_current_per_module_a * 10.0f);
    static int last_v_int = -1, last_i_int = -1;
    if (v_int != last_v_int || i_int != last_i_int) {
        last_v_int = v_int;
        last_i_int = i_int;
        LOG("CC: Standalone V=%d.%dV I=%d.%dA/mod\r\n",
            v_int / 10, v_int % 10, i_int / 10, i_int % 10);
    }

    apply_charge_targets();
}

static void run_bms_controlled_mode(uint32_t now_tick) {
    ChargeCycleConfig_t cfg;
    ChargeCycleConfig_Get(&cfg);

    BMS_View_t bms;
    BMS_GetView(&bms);

    /* Check BMS conditions */
    if (!bms.online) {
        LOG("CC: BMS offline\r\n");
        set_fault(CHARGE_CTRL_FAULT_BMS_OFFLINE, now_tick);
        return;
    }

    if (bms.alarm_flags & BMS_ALARM_STALE_DATA) {
        /* Stale data is a soft warning — keep running with last known targets.
         * Only log once to avoid spamming debug output. */
        if (!g_ctrl.bms_stale_warned) {
            LOG("CC: BMS stale data (keeping last targets)\r\n");
            g_ctrl.bms_stale_warned = true;
        }
        /* Don't fault — continue with previous targets */
    } else {
        g_ctrl.bms_stale_warned = false;
    }

    /* Check critical BMS alarms. BUGFIX 2026-08-29: this mask used to omit
     * BMS_ALARM_HIGH_PACK_VOLT and BMS_ALARM_TEMP_LOW_CHG, both of which
     * ARE in bms_critical_alarm_mask() (Modules/bms/bms_core.c) and so
     * already fail BMS_ShouldCloseChargeRelay() -- meaning either alarm
     * alone would make update_relay_decision() want the relay open while
     * the controller state stayed RUNNING (module still actively
     * sourcing current, nothing here telling it to stop). Kept in sync
     * with bms_critical_alarm_mask() by listing the same bits. */
    if (bms.alarm_flags & (BMS_ALARM_OVER_CHG_CURR | BMS_ALARM_HIGH_CELL_VOLT |
                           BMS_ALARM_TEMP_HIGH_CHG | BMS_ALARM_HIGH_PACK_VOLT |
                           BMS_ALARM_TEMP_LOW_CHG)) {
        LOG("CC: BMS alarm active\r\n");
        set_fault(CHARGE_CTRL_FAULT_BMS_ALARM, now_tick);
        return;
    }

    /* Base targets: Ignore BMS requests entirely, use local configuration.
     * Confirmed with the user 2026-08-29 (closes SRS TBD-04): this is the
     * intended design, not a gap. In BMS-Controlled mode the BMS's role is
     * monitoring/safety only (online/offline, critical alarms, cell-V/SOC
     * telemetry feeding the stage bands below, BMS_ShouldCloseChargeRelay()
     * for the relay) -- the actual V/I setpoint is always decided by the
     * charge algorithm from the locally-configured ChargeCycleConfig_t
     * (vmax_v / imax_c / stage bands), never by the BMS's own
     * ChgRequest_INFO (bms.chg_volt_request/bms.chg_curr_request are parsed
     * and available in BMS_View_t, but deliberately unused here).
     *
     * 2026-08-29, user-confirmed: while the relay hasn't armed yet
     * (relay_latched_closed still false), cap the commanded setpoint at
     * compute_voltage_ref() (the BMS's real pack voltage) instead of
     * jumping straight to the final vmax_v -- avoids commanding the
     * module toward a possibly much higher voltage while it's still
     * genuinely unloaded (relay open). Once relay_latched_closed flips
     * true, the real target takes over; apply_charge_targets() already
     * only sends a new CAN setpoint when target_voltage_v actually
     * changes, so this transition costs exactly one extra TX, not a
     * per-tick spam. */
    g_ctrl.target_voltage_v = g_ctrl.relay_latched_closed ? cfg.vmax_v : compute_voltage_ref(&cfg);

    /* Clamp voltage to hardware limits */
    if (g_ctrl.target_voltage_v > cfg.module_u_max_v) {
        g_ctrl.target_voltage_v = cfg.module_u_max_v;
    }

    /* Compute stage limits. This call is also what evaluates cell/SOC
     * completion (eval_cell_stage()/eval_soc_stage() latch
     * g_ctrl.cell_full_latched/soc_full_latched as a side effect) -- checked
     * immediately below, BEFORE any of stage_inhibit/stage_limit_source/etc
     * are used for anything, so a completed cycle never gets treated as an
     * ordinary current-gating inhibit even for one tick. See
     * apply_band_current_limit()'s doc comment for why cell/SOC's ABOVE_MAX
     * must NOT be read as a ChargeStageEval_t.inhibit like temperature's is. */
    float stage_limit_c = 0.0f;
    uint8_t stage_inhibit = 0;
    uint8_t stage_limit_source = CHARGE_LIMIT_SOURCE_NONE;
    uint8_t stage_band = CHARGE_STAGE_BAND_NONE;
    compute_stage_limits(&cfg, &bms, &stage_limit_c, &stage_inhibit,
                         &stage_limit_source, &stage_band);

    /* Completion check FIRST, ahead of the ordinary inhibit/derating fields
     * below -- cell voltage or SOC reaching its top band ends the charge
     * cycle outright (FR-CTRL-09/10), it is not merely "current=0, stay
     * RUNNING" like a temperature inhibit (FR-CTRL-11). Returning here means
     * g_ctrl.inhibit/active_limit_source/active_stage_band are deliberately
     * left untouched by this stage-limit read -- there is no "recoverable"
     * state to report once the cycle is ending. */
    if (g_ctrl.cell_full_latched || g_ctrl.soc_full_latched) {
        if (g_ctrl.cell_full_latched) {
            g_ctrl.stop_reason = CHARGE_STOP_CELL_VOLTAGE_REACHED;
        } else {
            g_ctrl.stop_reason = CHARGE_STOP_SOC_REACHED;
        }
        g_ctrl.target_current_total_a = 0.0f;
        g_ctrl.target_current_per_module_a = 0.0f;
        transition_to(CHARGE_CTRL_STATE_STOPPING, now_tick);
        return;
    }

    /* Not completing: stage_inhibit here is a genuine, recoverable
     * current-gating inhibit (temperature out of range, or cell/SOC still
     * below their configured minimum). */
    g_ctrl.inhibit = stage_inhibit;
    g_ctrl.derating = 0;
    g_ctrl.active_limit_source = stage_limit_source;
    g_ctrl.active_stage_band = stage_band;
    g_ctrl.active_limit_current_c = stage_limit_c;

    /* Calculate current */
    if (g_ctrl.inhibit) {
        g_ctrl.target_current_total_a = 0.0f;
    } else {
        /* Use BMS rated capacity if available, otherwise fallback to config */
        float active_capacity = (bms.rate_cap > 0) ? ((float)bms.rate_cap * 0.1f) : cfg.battery_capacity_ah;

        /* Convert C-rate to Amps */
        float stage_limit_a = stage_limit_c * active_capacity;

        /* Start with maximum configured current */
        float max_allowed_a = cfg.imax_c * active_capacity;
        g_ctrl.target_current_total_a = max_allowed_a;

        if (stage_limit_a < g_ctrl.target_current_total_a) {
            g_ctrl.target_current_total_a = stage_limit_a;
            g_ctrl.derating = 1;
        }

        apply_jack_temp_derating(&cfg, now_tick);
    }

    /* Per-module split (strict: must match configured count) */
    if (g_ctrl.actual_module_count > 0) {
        g_ctrl.target_current_per_module_a = g_ctrl.target_current_total_a / (float)g_ctrl.actual_module_count;
    } else {
        g_ctrl.target_current_per_module_a = 0.0f;
    }

    /* Clamp to module_i_max_a (downward only) */
    if (g_ctrl.target_current_per_module_a > cfg.module_i_max_a) {
        g_ctrl.target_current_per_module_a = cfg.module_i_max_a;
        /* Recalculate total current to match clamped per-module value */
        g_ctrl.target_current_total_a = g_ctrl.target_current_per_module_a * (float)g_ctrl.actual_module_count;
        g_ctrl.derating = 1;
    }

    /* Note: No upward clamp to module_i_min_a */

    apply_charge_targets();
}

/* ============== Public API ============== */

void ChargeController_Init(void) {
    memset(&g_ctrl, 0, sizeof(g_ctrl));
    g_ctrl.state = CHARGE_CTRL_STATE_IDLE;
    g_ctrl.owner = CHARGE_CTRL_OWNER_NONE;
    /* Initialize last_* tracking to ensure first state change is logged */
    g_ctrl.last_inhibit = 1;
    g_ctrl.last_derating = 1;
    g_ctrl.last_running = 0;
    g_ctrl.stop_reason = CHARGE_STOP_NONE;
    /* Same "no sensor reading yet" fallback apply_jack_temp_derating used
     * to apply itself when all 4 NTC channels read invalid. */
    g_ctrl.jack_temp_input_c = 25.0f;
    LOG("CC: Init\r\n");
}

void ChargeController_SetJackTempC(float temp_c) {
    if (isfinite(temp_c)) {
        g_ctrl.jack_temp_input_c = temp_c;
    }
}

void ChargeController_Process(uint32_t now_tick) {
    /* Update actual module count */
    g_ctrl.actual_module_count = get_active_module_count();

    /* Check for module count mismatch during running */
    if (g_ctrl.state == CHARGE_CTRL_STATE_RUNNING &&
        g_ctrl.actual_module_count != g_ctrl.source_module_count) {
        
        if (g_ctrl.module_mismatch_timer_tick == 0) {
            g_ctrl.module_mismatch_timer_tick = now_tick;
        } else if (now_tick - g_ctrl.module_mismatch_timer_tick >= 10000) {
            LOG("CC: Module mismatch %u->%u\r\n",
                (unsigned)g_ctrl.source_module_count, (unsigned)g_ctrl.actual_module_count);
            set_fault(CHARGE_CTRL_FAULT_MODULE_COUNT_MISMATCH, now_tick);
        }
    } else {
        g_ctrl.module_mismatch_timer_tick = 0;
    }

    switch (g_ctrl.state) {
        case CHARGE_CTRL_STATE_IDLE:
            /* Nothing to do */
            break;

        case CHARGE_CTRL_STATE_READY:
            /* Check preconditions and start */
            if (check_preconditions_set_fault(now_tick)) {
                transition_to(CHARGE_CTRL_STATE_RUNNING, now_tick);
            } else {
                transition_to(CHARGE_CTRL_STATE_FAULT, now_tick);
            }
            break;

        case CHARGE_CTRL_STATE_RUNNING: {
            ChargeCycleConfig_t cfg;
            BMS_View_t bms;
            ChargeCycleConfig_Get(&cfg);
            BMS_GetView(&bms);

            /* Update hard protection timers FIRST
             * Note: This may trigger FAULT, which changes g_ctrl.state.
             * If fault was triggered, skip mode handler to prevent
             * apply_charge_targets() in the same cycle. */
            ChargeCtrlState_t state_before_protection = g_ctrl.state;
            update_hard_protection(&cfg, &bms, now_tick);

            /* Only run mode handler if we're still in RUNNING/DERATING */
            if (g_ctrl.state == state_before_protection) {
                if (g_ctrl.manual_mode) {
                    run_manual_mode(now_tick);
                } else if (cfg.charge_source_mode == CHARGE_SOURCE_STANDALONE_NO_BMS) {
                    run_standalone_mode(now_tick);
                } else {
                    run_bms_controlled_mode(now_tick);
                }

                /* Derating is an output condition, not a second runtime
                 * state. Keep the controller in RUNNING so stop/start logic
                 * has one normal operating path. */
            } else {
                /* Hard protection triggered FAULT - stop immediately */
                LOG("CC: Hard protection FAULT\r\n");
                stop_charging();
                /* Clear protection timers */
                g_ctrl.protect_jack_v_timer_tick = 0;
                g_ctrl.protect_jack_temp_timer_tick = 0;
            }
            break;
        }

        case CHARGE_CTRL_STATE_STOPPING:
            stop_charging();
            /* Clear protection timers */
            g_ctrl.protect_jack_v_timer_tick = 0;
            g_ctrl.protect_jack_temp_timer_tick = 0;
            transition_to(CHARGE_CTRL_STATE_IDLE, now_tick);
            g_ctrl.owner = CHARGE_CTRL_OWNER_NONE;
            break;

        case CHARGE_CTRL_STATE_FAULT:
            stop_charging();
            /* Clear protection timers */
            g_ctrl.protect_jack_v_timer_tick = 0;
            g_ctrl.protect_jack_temp_timer_tick = 0;
            /* Stay in fault until explicitly cleared */
            break;
    }

    update_relay_decision(now_tick);
    update_bms_charge_allow();
}

bool ChargeController_CheckPreconditions(uint32_t *fault_flags_out) {
    uint32_t faults = check_preconditions_faults();
    if (fault_flags_out != NULL) {
        *fault_flags_out = faults;
    }
    return (faults == CHARGE_CTRL_FAULT_NONE);
}

bool ChargeController_Start(ChargeCtrlOwner_t owner, bool manual_mode, uint32_t now_tick) {
    if (g_ctrl.state == CHARGE_CTRL_STATE_RUNNING) {
        LOG("CC: Already running\r\n");
        return true;  /* Already running */
    }

    /* Check preconditions BEFORE transitioning to READY */
    uint32_t faults = check_preconditions_faults();
    if (faults != CHARGE_CTRL_FAULT_NONE) {
        LOG("CC: Preconditions failed fault=0x%08lX\r\n", (unsigned long)faults);
        g_ctrl.fault_flags = faults;
        g_ctrl.stop_reason = CHARGE_STOP_PRECONDITION;
        transition_to(CHARGE_CTRL_STATE_FAULT, now_tick);
        return false;
    }

    /* FIX: Initialize module counts (was missing - caused bug where modules don't start) */
    ChargeCycleConfig_t cfg;
    ChargeCycleConfig_Get(&cfg);
    g_ctrl.source_module_count = cfg.source_module_count;
    g_ctrl.actual_module_count = get_active_module_count();
    LOG("CC: Start src=%u act=%u\r\n",
        (unsigned)g_ctrl.source_module_count, (unsigned)g_ctrl.actual_module_count);

    g_ctrl.owner = owner;
    g_ctrl.manual_mode = manual_mode;
    g_ctrl.stop_reason = CHARGE_STOP_NONE;
    g_ctrl.standalone_vmax_reached_tick = 0;
    
    /* Reset hysteresis tracking bands for the new cycle */
    g_ctrl.last_cell_band = CHARGE_STAGE_BAND_NONE;
    g_ctrl.last_temp_band = CHARGE_STAGE_BAND_NONE;
    g_ctrl.last_soc_band = CHARGE_STAGE_BAND_NONE;
    g_ctrl.max_cell_band = CHARGE_STAGE_BAND_NONE;
    g_ctrl.max_soc_band = CHARGE_STAGE_BAND_NONE;
    g_ctrl.cell_full_latched = false;
    g_ctrl.soc_full_latched = false;
    g_ctrl.last_running = 0;

    clear_fault();

    transition_to(CHARGE_CTRL_STATE_READY, now_tick);
    return true;
}

void ChargeController_SetManualTarget(float voltage, float current) {
    if (!isfinite(voltage) || !isfinite(current)) return;
    g_ctrl.manual_target_voltage_v = voltage;
    g_ctrl.manual_target_current_per_module_a = current;
}

bool ChargeController_IsManualMode(void) {
    return g_ctrl.manual_mode;
}

void ChargeController_Stop(uint32_t now_tick) {
    if (g_ctrl.state == CHARGE_CTRL_STATE_IDLE) {
        return;
    }

    LOG("CC: Stop requested\r\n");
    g_ctrl.stop_reason = CHARGE_STOP_USER_COMMAND;

    if (g_ctrl.state == CHARGE_CTRL_STATE_FAULT) {
        /* Clear fault and go to IDLE */
        clear_fault();
        transition_to(CHARGE_CTRL_STATE_IDLE, now_tick);
        g_ctrl.owner = CHARGE_CTRL_OWNER_NONE;
    } else {
        transition_to(CHARGE_CTRL_STATE_STOPPING, now_tick);
    }
}

void ChargeController_EmergencyStop(uint32_t now_tick) {
    LOG("CC: EMERGENCY STOP\r\n");

    /* Immediate hardware stop */
    CHG_LIB_EmergencyStop();

    /* Set fault flag */
    set_fault(CHARGE_CTRL_FAULT_EMERGENCY_STOP, now_tick);

    /* Force to fault state */
    transition_to(CHARGE_CTRL_STATE_FAULT, now_tick);

    g_ctrl.owner = CHARGE_CTRL_OWNER_NONE;
}

bool ChargeController_IsRunning(void) {
    return (g_ctrl.state == CHARGE_CTRL_STATE_RUNNING);
}

void ChargeController_AcknowledgeCompletion(void) {
    /* Only meaningful once a normal finish has parked us at IDLE. Clearing
     * the stop_reason marker is purely informational -- relay/fault/state
     * are untouched -- so the HMI (dwin_status_from_state) and the PC view
     * stop reporting COMPLETE and the button reverts to START. */
    if (g_ctrl.state != CHARGE_CTRL_STATE_IDLE) {
        return;
    }
    switch (g_ctrl.stop_reason) {
        case CHARGE_STOP_VOLTAGE_REACHED:
        case CHARGE_STOP_CELL_VOLTAGE_REACHED:
        case CHARGE_STOP_SOC_REACHED:
            /* owner is already NONE here (STOPPING->IDLE cleared it). */
            g_ctrl.stop_reason = CHARGE_STOP_NONE;
            LOG("CC: completion acknowledged -> READY\r\n");
            break;
        default:
            break;
    }
}

void ChargeController_GetView(ChargeCtrlView_t *view) {
    view->state = g_ctrl.state;
    view->running = ChargeController_IsRunning();
    view->faulted = (g_ctrl.state == CHARGE_CTRL_STATE_FAULT);
    view->derating = g_ctrl.derating;
    view->inhibit = g_ctrl.inhibit;
    view->start_requested = (g_ctrl.state == CHARGE_CTRL_STATE_READY);
    view->emergency_stop = (g_ctrl.fault_flags & CHARGE_CTRL_FAULT_EMERGENCY_STOP) ? 1 : 0;
    view->target_voltage_v = g_ctrl.target_voltage_v;
    view->target_current_total_a = g_ctrl.target_current_total_a;
    view->target_current_per_module_a = g_ctrl.target_current_per_module_a;
    view->applied_voltage_v = g_ctrl.applied_voltage_v;
    view->applied_current_per_module_a = g_ctrl.applied_current_per_module_a;
    view->fault_flags = g_ctrl.fault_flags;
    view->last_update_tick = g_ctrl.last_update_tick;
    view->owner = g_ctrl.owner;
    view->source_module_count = g_ctrl.source_module_count;
    view->actual_module_count = g_ctrl.actual_module_count;

    ChargeCycleConfig_t cfg;
    ChargeCycleConfig_Get(&cfg);
    view->charge_source_mode = cfg.charge_source_mode;
    view->active_limit_source = g_ctrl.active_limit_source;
    view->active_stage_band = g_ctrl.active_stage_band;
    view->active_limit_current_c = g_ctrl.active_limit_current_c;
    view->stop_reason = g_ctrl.stop_reason;
    view->relay_should_close = g_ctrl.relay_should_close ? 1U : 0U;
}




