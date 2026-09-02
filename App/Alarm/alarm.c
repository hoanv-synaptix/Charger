/**
 * @file  alarm.c
 * @brief Unified alarm subsystem implementation -- see alarm.h.
 *
 * One spec-table row per alarm code (same table-driven shape as
 * Modules/bms/bms_protocol.c's frame dispatch). Each tick:
 *   1. snapshot the controller / BMS / module views once (AlarmInputs_t)
 *   2. evaluate every row's raw condition, run its set/clear debounce
 *   3. aggregate active alarms -> AlarmView_t
 *   4. edge-trigger one stop / emergency-stop through ChargeController_*
 *
 * Tuning constants below are STARTING VALUES -- they gate relay / fault
 * behaviour and have not been checked against a real DC bus / scope, same
 * status as RELAY_OPEN_TIMEOUT_MS / RS485 TX timeout / POWER_EN delay
 * (CLAUDE.md sec 6, AUDIT I-07/I-10). Marked "HW-TBD".
 */

#include "alarm.h"

#include "charge_controller.h"
#include "charge_cycle_config.h"
#include "bms_core.h"
#include "chg_lib.h"
#include "debug_log.h"

#include <string.h>
#include <math.h>

/* ============== Tuning (HW-TBD: validate on a real bus) ============== */

/* Per-module "meaningful" charge current. Below this we treat the DC path as
 * carrying no load. Well above CAN quantization noise, small vs typical
 * 50-100 A module ratings. */
#define ALARM_I_LOAD_MIN_A              2.0f

/* DC load-loss: measured current has collapsed to below this fraction of the
 * commanded per-module current while the commanded voltage is still pinned at
 * target -> the battery path opened (hot unplug / external contactor drop). */
#define ALARM_LOAD_LOST_I_FRAC         0.15f
#define ALARM_V_AT_TARGET_FRAC         0.98f
#define ALARM_LOAD_LOST_MS            800U

/* After the battery relay is commanded closed, real current must establish
 * within this window; if not, the external "relay sac" never closed. */
#define ALARM_DC_OUT_CONFIRM_MS      5000U

/* BMS online but pack terminal voltage below this fraction of the configured
 * pack Vmax -> no battery actually connected (pack contactor / BMS relay-pin
 * open, or connector not seated). */
#define ALARM_V_PACK_FLOOR_FRAC       0.5f

/* AC input, per phase. FLOOR distinguishes "phase lost" from merely low;
 * EPS distinguishes "module not reporting AC" (leave alone) from a real 0 V. */
#define ALARM_AC_PHASE_EPS_V          20.0f
#define ALARM_AC_PHASE_FLOOR_V       150.0f
#define ALARM_AC_PHASE_LOSS_FRAC      0.5f
#define ALARM_AC_UNDERVOLT_V         170.0f

/* Debounce windows */
#define ALARM_DB_MIRROR_CLEAR_MS      200U   /* source is already debounced   */
#define ALARM_DB_COMM_SET_MS          200U
#define ALARM_DB_COMM_CLEAR_MS        500U
#define ALARM_DB_NO_PACK_SET_MS       500U
#define ALARM_DB_NO_PACK_CLEAR_MS    1000U
#define ALARM_DB_AC_MS               1000U

/* Console (LOG) breadcrumb rate-limit. LOG() blocks up to 50 ms
 * (Utils/Log/debug_log.c); several alarms can flip in a single 20 ms tick
 * (e.g. a BMS dropping out sets its mirror bits + BMS_COMM_LOST at once), so
 * emit at most ONE consolidated line per tick and no more than one line per
 * this interval. The RAM event-log ring + the PC DEBUG_RSP_ALARMS readout keep
 * the full per-edge history regardless -- the console line is only a bench aid. */
#define ALARM_LOG_MIN_INTERVAL_MS    1000U

/* ============== Inputs snapshot ============== */

typedef struct {
    uint32_t now;
    ChargeCtrlView_t cc;
    BMS_View_t bms;

    /* aggregated over enabled+online modules */
    uint8_t  mod_online_count;
    float    mod_current_max;   /* -1 if none reporting */
    float    mod_voltage_min;   /* -1 if none reporting */
    uint32_t mod_alarm_or;
    uint8_t  mod_pfc_fault_or;
    bool     ac_phase_loss;
    bool     ac_undervolt;

    float    cfg_vmax_v;
    uint8_t  cfg_source_mode;
} AlarmInputs_t;

/* ============== Per-code runtime state ============== */

typedef struct {
    bool     raw_prev;
    uint32_t edge_tick;   /* last raw-state change */
    bool     active;
    bool     latched;     /* latching alarm whose raw condition has cleared */
} AlarmRt_t;

/* ============== Spec table ============== */

/* Plain C function pointer -- each spec row's raw-condition predicate. Not an
 * interpreter/eval of any kind; the table is fully static (k_specs below). */
typedef bool (*AlarmEvalFn)(const AlarmInputs_t *in, uint32_t param);

typedef struct {
    AlarmCode_t   code;
    AlarmAction_t action;
    bool          latching;
    uint16_t      set_ms;
    uint16_t      clear_ms;
    AlarmEvalFn   eval;
    uint32_t      param;
    const char   *desc;
} AlarmSpec_t;

/* ---- generic mirror evals ---- */

static bool ev_bms(const AlarmInputs_t *in, uint32_t bit) {
    return (in->bms.alarm_flags & bit) != 0U;
}
static bool ev_mod(const AlarmInputs_t *in, uint32_t bit) {
    return (in->mod_alarm_or & bit) != 0U;
}
static bool ev_mod_pfc(const AlarmInputs_t *in, uint32_t param) {
    (void)param;
    return in->mod_pfc_fault_or != 0U;
}
static bool ev_ctrl(const AlarmInputs_t *in, uint32_t bit) {
    return (in->cc.fault_flags & bit) != 0U;
}

/* ---- derived evals (forward decl; defined after g_alarm) ---- */
static bool ev_bms_comm_lost(const AlarmInputs_t *in, uint32_t param);
static bool ev_bms_no_pack_voltage(const AlarmInputs_t *in, uint32_t param);
static bool ev_dc_load_lost(const AlarmInputs_t *in, uint32_t param);
static bool ev_dc_out_not_established(const AlarmInputs_t *in, uint32_t param);
static bool ev_ac_phase_loss(const AlarmInputs_t *in, uint32_t param);
static bool ev_ac_undervolt(const AlarmInputs_t *in, uint32_t param);

/* BMS severity: bits that gate the charge relay in bms_core.c
 * (bms_critical_alarm_mask). Keep this list in sync with that function --
 * mirrored here, not shared, same as charge_controller.c's copy. */

static const AlarmSpec_t k_specs[] = {
    /* code, action, latch, set_ms, clear_ms, eval, param, desc */

    /* --- BMS-reported --- */
    { ALARM_BMS_LOW_PACK_VOLT,   ALARM_ACT_INFO,  false, 0, ALARM_DB_MIRROR_CLEAR_MS, ev_bms, BMS_ALARM_LOW_PACK_VOLT,   "BMS low pack voltage" },
    { ALARM_BMS_LOW_CELL_VOLT,   ALARM_ACT_INFO,  false, 0, ALARM_DB_MIRROR_CLEAR_MS, ev_bms, BMS_ALARM_LOW_CELL_VOLT,   "BMS low cell voltage" },
    { ALARM_BMS_HIGH_PACK_VOLT,  ALARM_ACT_STOP,  false, 0, ALARM_DB_MIRROR_CLEAR_MS, ev_bms, BMS_ALARM_HIGH_PACK_VOLT,  "BMS high pack voltage" },
    { ALARM_BMS_HIGH_CELL_VOLT,  ALARM_ACT_STOP,  false, 0, ALARM_DB_MIRROR_CLEAR_MS, ev_bms, BMS_ALARM_HIGH_CELL_VOLT,  "BMS high cell voltage" },
    { ALARM_BMS_TEMP_HIGH_CHG,   ALARM_ACT_STOP,  false, 0, ALARM_DB_MIRROR_CLEAR_MS, ev_bms, BMS_ALARM_TEMP_HIGH_CHG,   "BMS charge over-temp" },
    { ALARM_BMS_TEMP_HIGH_DCHG,  ALARM_ACT_INFO,  false, 0, ALARM_DB_MIRROR_CLEAR_MS, ev_bms, BMS_ALARM_TEMP_HIGH_DCHG,  "BMS discharge over-temp" },
    { ALARM_BMS_TEMP_LOW_CHG,    ALARM_ACT_STOP,  false, 0, ALARM_DB_MIRROR_CLEAR_MS, ev_bms, BMS_ALARM_TEMP_LOW_CHG,    "BMS charge under-temp" },
    { ALARM_BMS_TEMP_LOW_DCHG,   ALARM_ACT_INFO,  false, 0, ALARM_DB_MIRROR_CLEAR_MS, ev_bms, BMS_ALARM_TEMP_LOW_DCHG,   "BMS discharge under-temp" },
    { ALARM_BMS_TEMP_RELAY_HIGH, ALARM_ACT_INFO,  false, 0, ALARM_DB_MIRROR_CLEAR_MS, ev_bms, BMS_ALARM_TEMP_RELAY_HIGH, "BMS relay over-temp" },
    { ALARM_BMS_OVER_CHG_CURR,   ALARM_ACT_STOP,  false, 0, ALARM_DB_MIRROR_CLEAR_MS, ev_bms, BMS_ALARM_OVER_CHG_CURR,   "BMS over charge current" },
    { ALARM_BMS_OVER_DCHG_CURR,  ALARM_ACT_INFO,  false, 0, ALARM_DB_MIRROR_CLEAR_MS, ev_bms, BMS_ALARM_OVER_DCHG_CURR,  "BMS over discharge current" },
    { ALARM_BMS_CELL_VOLT_DIFF,  ALARM_ACT_INFO,  false, 0, ALARM_DB_MIRROR_CLEAR_MS, ev_bms, BMS_ALARM_CELL_VOLT_DIFF,  "BMS cell voltage imbalance" },
    { ALARM_BMS_LOW_SOC,         ALARM_ACT_INFO,  false, 0, ALARM_DB_MIRROR_CLEAR_MS, ev_bms, BMS_ALARM_LOW_SOC,         "BMS low SOC" },

    /* --- module-reported --- */
    { ALARM_MOD_HW_FAULT,        ALARM_ACT_STOP,  false, 0, ALARM_DB_MIRROR_CLEAR_MS, ev_mod, CHG_LIB_ALARM_HW_FAULT,         "Module hardware fault" },
    { ALARM_MOD_COMM_FAIL,       ALARM_ACT_INFO,  false, 0, ALARM_DB_MIRROR_CLEAR_MS, ev_mod, CHG_LIB_ALARM_COMM_FAIL,        "Module comms fail" },
    { ALARM_MOD_OVER_TEMP,       ALARM_ACT_STOP,  false, 0, ALARM_DB_MIRROR_CLEAR_MS, ev_mod, CHG_LIB_ALARM_OVER_TEMP,        "Module over-temp" },
    { ALARM_MOD_OVER_VOLT_OUT,   ALARM_ACT_ESTOP, false, 0, ALARM_DB_MIRROR_CLEAR_MS, ev_mod, CHG_LIB_ALARM_OVER_VOLTAGE_OUT, "Module output over-voltage" },
    { ALARM_MOD_SHORT_CIRCUIT,   ALARM_ACT_ESTOP, false, 0, ALARM_DB_MIRROR_CLEAR_MS, ev_mod, CHG_LIB_ALARM_SHORT_CIRCUIT,    "Module output short circuit" },
    { ALARM_MOD_AC_UNDER_VOLT,   ALARM_ACT_INFO,  false, 0, ALARM_DB_MIRROR_CLEAR_MS, ev_mod, CHG_LIB_ALARM_AC_UNDER_VOLT,    "Module AC under-voltage" },
    { ALARM_MOD_OVER_CURR_OUT,   ALARM_ACT_STOP,  false, 0, ALARM_DB_MIRROR_CLEAR_MS, ev_mod, CHG_LIB_ALARM_OVER_CURR_OUT,    "Module output over-current" },
    { ALARM_MOD_PFC_FAULT,       ALARM_ACT_STOP,  false, 0, ALARM_DB_MIRROR_CLEAR_MS, ev_mod_pfc, 0,                          "Module PFC fault" },

    /* --- controller faults (mirror, report/log only -- controller already acts) --- */
    { ALARM_CTRL_NO_MODULE,       ALARM_ACT_INFO, false, 0, ALARM_DB_MIRROR_CLEAR_MS, ev_ctrl, CHARGE_CTRL_FAULT_NO_MODULE,             "No charger module" },
    { ALARM_CTRL_MODULE_MISMATCH, ALARM_ACT_INFO, false, 0, ALARM_DB_MIRROR_CLEAR_MS, ev_ctrl, CHARGE_CTRL_FAULT_MODULE_COUNT_MISMATCH, "Module count mismatch" },
    { ALARM_CTRL_INVALID_CONFIG,  ALARM_ACT_INFO, false, 0, ALARM_DB_MIRROR_CLEAR_MS, ev_ctrl, CHARGE_CTRL_FAULT_INVALID_CONFIG,        "Invalid charge config" },
    { ALARM_CTRL_JACK_OVER_V,     ALARM_ACT_INFO, false, 0, ALARM_DB_MIRROR_CLEAR_MS, ev_ctrl, CHARGE_CTRL_FAULT_PROTECT_JACK_V,        "Connector over-voltage protect" },
    { ALARM_CTRL_JACK_OVER_TEMP,  ALARM_ACT_INFO, false, 0, ALARM_DB_MIRROR_CLEAR_MS, ev_ctrl, CHARGE_CTRL_FAULT_PROTECT_JACK_TEMP,     "Connector over-temp protect" },
    { ALARM_CTRL_EMERGENCY_STOP,  ALARM_ACT_INFO, false, 0, ALARM_DB_MIRROR_CLEAR_MS, ev_ctrl, CHARGE_CTRL_FAULT_EMERGENCY_STOP,        "Emergency stop" },

    /* --- derived / station-level --- */
    { ALARM_BMS_COMM_LOST,          ALARM_ACT_INFO, false, ALARM_DB_COMM_SET_MS,    ALARM_DB_COMM_CLEAR_MS,    ev_bms_comm_lost,          0, "BMS link lost during charge" },
    { ALARM_BMS_NO_PACK_VOLTAGE,    ALARM_ACT_STOP, false, ALARM_DB_NO_PACK_SET_MS, ALARM_DB_NO_PACK_CLEAR_MS, ev_bms_no_pack_voltage,    0, "No pack voltage" },
    { ALARM_DC_LOAD_LOST,           ALARM_ACT_STOP, true,  ALARM_LOAD_LOST_MS,      0,                        ev_dc_load_lost,           0, "DC load lost" },
    { ALARM_DC_OUT_NOT_ESTABLISHED, ALARM_ACT_STOP, true,  0,                       0,                        ev_dc_out_not_established, 0, "DC output not established" },
    { ALARM_AC_PHASE_LOSS,          ALARM_ACT_STOP, true,  ALARM_DB_AC_MS,          ALARM_DB_AC_MS,           ev_ac_phase_loss,          0, "AC input phase loss" },
    { ALARM_AC_UNDERVOLT,           ALARM_ACT_STOP, true,  ALARM_DB_AC_MS,          ALARM_DB_AC_MS,           ev_ac_undervolt,           0, "AC input under-voltage" },
};

#define ALARM_SPEC_COUNT ((uint8_t)(sizeof(k_specs) / sizeof(k_specs[0])))

/* ============== Module state ============== */

static struct {
    AlarmRt_t rt[ALARM_SPEC_COUNT];

    /* session trackers for the DC derived alarms */
    bool     prev_relay_close;
    uint32_t relay_close_since;   /* 0 = relay open */
    bool     load_established;    /* saw real current this RUNNING session */

    /* edge-triggered dispatch latches */
    bool stop_sent;
    bool estop_sent;

    /* event log ring */
    AlarmLogEntry_t log[ALARM_LOG_DEPTH];
    uint8_t  log_head;   /* next write index */
    uint8_t  log_count;

    AlarmView_t view;
    uint32_t last_console_log_tick;
    bool     inited;
} g_alarm;

/* Per-tick edge tally, so run_debounce() does no blocking LOG() itself. */
typedef struct {
    uint8_t      raised;
    uint8_t      cleared;
    const char  *first_raised_desc;
    const char  *first_cleared_desc;
} AlarmEdgeTally_t;

/* ============== Derived evals ============== */

/* The charge controller reacts to a lost BMS link itself (run_bms_controlled_mode
 * -> set_fault(CHARGE_CTRL_FAULT_BMS_OFFLINE) -> FAULT) usually before this
 * eval sees state==RUNNING, so also accept the controller's own conclusion.
 * Action is INFO: the controller already stopped, this alarm only labels/logs
 * the reason ("mat ket noi BMS"). */
static bool ev_bms_comm_lost(const AlarmInputs_t *in, uint32_t param) {
    (void)param;
    if (in->cfg_source_mode != CHARGE_SOURCE_BMS_CONTROLLED) return false;
    if (in->bms.online) return false;
    return (in->cc.state == CHARGE_CTRL_STATE_RUNNING) ||
           (in->cc.state == CHARGE_CTRL_STATE_READY) ||
           ((in->cc.fault_flags & CHARGE_CTRL_FAULT_BMS_OFFLINE) != 0U);
}

static bool ev_bms_no_pack_voltage(const AlarmInputs_t *in, uint32_t param) {
    (void)param;
    if (in->cfg_source_mode != CHARGE_SOURCE_BMS_CONTROLLED) return false;
    if (!in->bms.online) return false;
    if (in->cc.state != CHARGE_CTRL_STATE_RUNNING &&
        in->cc.state != CHARGE_CTRL_STATE_READY) return false;
    if (in->cfg_vmax_v <= 0.0f) return false;
    return in->bms.batt_voltage < (in->cfg_vmax_v * ALARM_V_PACK_FLOOR_FRAC);
}

/* Distinguished from a normal CV-phase current taper by TWO extra conditions:
 * the commanded voltage is still pinned at target AND (BMS mode) the BMS's own
 * pack current has also fallen to ~0. A genuine taper keeps batt_current > 0
 * and decays smoothly; an opened DC path drops both to zero at once while the
 * module output voltage jumps to its ceiling. */
static bool ev_dc_load_lost(const AlarmInputs_t *in, uint32_t param) {
    (void)param;
    if (in->cc.state != CHARGE_CTRL_STATE_RUNNING) return false;
    if (!g_alarm.load_established) return false;
    if (in->cc.applied_current_per_module_a <= ALARM_I_LOAD_MIN_A) return false;
    if (in->mod_current_max < 0.0f) return false;
    if (in->mod_current_max >=
        (in->cc.applied_current_per_module_a * ALARM_LOAD_LOST_I_FRAC)) return false;
    if (in->mod_voltage_min < 0.0f) return false;
    if (in->mod_voltage_min <= (in->cc.target_voltage_v * ALARM_V_AT_TARGET_FRAC)) return false;
    if (in->cfg_source_mode == CHARGE_SOURCE_BMS_CONTROLLED &&
        fabsf(in->bms.batt_current) >= ALARM_I_LOAD_MIN_A) return false;
    return true;
}

static bool ev_dc_out_not_established(const AlarmInputs_t *in, uint32_t param) {
    (void)param;
    if (in->cc.state != CHARGE_CTRL_STATE_RUNNING) return false;
    if (!in->cc.relay_should_close) return false;
    if (g_alarm.load_established) return false;
    if (g_alarm.relay_close_since == 0U) return false;
    if ((in->now - g_alarm.relay_close_since) < ALARM_DC_OUT_CONFIRM_MS) return false;
    if (in->cc.applied_current_per_module_a <= ALARM_I_LOAD_MIN_A) return false;
    return (in->mod_current_max >= 0.0f) && (in->mod_current_max < ALARM_I_LOAD_MIN_A);
}

static bool ev_ac_phase_loss(const AlarmInputs_t *in, uint32_t param) {
    (void)param;
    return in->ac_phase_loss;
}

static bool ev_ac_undervolt(const AlarmInputs_t *in, uint32_t param) {
    (void)param;
    return in->ac_undervolt;
}

/* ============== Inputs gather ============== */

static void gather_inputs(uint32_t now, AlarmInputs_t *in) {
    memset(in, 0, sizeof(*in));
    in->now = now;
    ChargeController_GetView(&in->cc);
    BMS_GetView(&in->bms);

    ChargeCycleConfig_t cfg;
    ChargeCycleConfig_Get(&cfg);
    in->cfg_vmax_v = cfg.vmax_v;
    in->cfg_source_mode = cfg.charge_source_mode;

    in->mod_current_max = -1.0f;
    in->mod_voltage_min = -1.0f;

    uint8_t total = CHG_LIB_GetModuleCount();
    for (uint8_t i = 0; i < total; i++) {
        CHG_LIB_ModuleView_t mv;
        if (!CHG_LIB_GetModuleView(i, &mv)) continue;
        if (!mv.enabled || !mv.online) continue;
        if (mv.state == CHG_LIB_STATE_OFFLINE || mv.state == CHG_LIB_STATE_FAULT) {
            /* still fold its alarm bits in -- a faulted module's cause matters */
            in->mod_alarm_or |= (uint32_t)mv.alarm_flags;
            in->mod_pfc_fault_or |= mv.pfc_fault;
            continue;
        }

        in->mod_online_count++;
        in->mod_alarm_or |= (uint32_t)mv.alarm_flags;
        in->mod_pfc_fault_or |= mv.pfc_fault;

        if (isfinite(mv.current) && mv.current > in->mod_current_max) {
            in->mod_current_max = mv.current;
        }
        if (isfinite(mv.voltage) &&
            (in->mod_voltage_min < 0.0f || mv.voltage < in->mod_voltage_min)) {
            in->mod_voltage_min = mv.voltage;
        }

        /* AC phase analysis for this module */
        float pa = mv.ac_phase_a_voltage, pb = mv.ac_phase_b_voltage, pc = mv.ac_phase_c_voltage;
        float pmax = pa; if (pb > pmax) pmax = pb; if (pc > pmax) pmax = pc;
        float pmin = pa; if (pb < pmin) pmin = pb; if (pc < pmin) pmin = pc;

        if (isfinite(pmax) && pmax > ALARM_AC_PHASE_EPS_V) {
            /* module is actually reporting AC -- worth judging */
            if (pmin < (ALARM_AC_PHASE_LOSS_FRAC * pmax) &&
                pmin < ALARM_AC_PHASE_FLOOR_V &&
                pmax > ALARM_AC_PHASE_FLOOR_V) {
                in->ac_phase_loss = true;
            }
            if (pa > ALARM_AC_PHASE_EPS_V && pa < ALARM_AC_UNDERVOLT_V &&
                pb > ALARM_AC_PHASE_EPS_V && pb < ALARM_AC_UNDERVOLT_V &&
                pc > ALARM_AC_PHASE_EPS_V && pc < ALARM_AC_UNDERVOLT_V) {
                in->ac_undervolt = true;
            }
        }
    }

    /* module's own AC-undervoltage bit is a fast path */
    if (in->mod_alarm_or & CHG_LIB_ALARM_AC_UNDER_VOLT) {
        in->ac_undervolt = true;
    }
}

/* ============== Event log ============== */

static void log_edge(uint32_t now, AlarmCode_t code, AlarmAction_t action, bool raised) {
    AlarmLogEntry_t *e = &g_alarm.log[g_alarm.log_head];
    e->uptime_ms = now;
    e->code = (uint16_t)code;
    e->action = (uint8_t)action;
    e->event = raised ? 1U : 0U;
    g_alarm.log_head = (uint8_t)((g_alarm.log_head + 1U) % ALARM_LOG_DEPTH);
    if (g_alarm.log_count < ALARM_LOG_DEPTH) g_alarm.log_count++;
}

/* ============== Debounce + aggregate ============== */

static void run_debounce(uint32_t now, const AlarmInputs_t *in, AlarmEdgeTally_t *tally) {
    for (uint8_t i = 0; i < ALARM_SPEC_COUNT; i++) {
        const AlarmSpec_t *sp = &k_specs[i];
        AlarmRt_t *rt = &g_alarm.rt[i];

        bool raw = sp->eval(in, sp->param);

        if (raw != rt->raw_prev) {
            rt->raw_prev = raw;
            rt->edge_tick = now;
        }
        uint32_t held = now - rt->edge_tick;

        if (raw) {
            if (rt->latched) rt->latched = false; /* condition returned -- genuinely active */
            if (!rt->active && held >= sp->set_ms) {
                rt->active = true;
                log_edge(now, sp->code, sp->action, true);
                if (tally->raised++ == 0U) tally->first_raised_desc = sp->desc;
            }
        } else {
            if (rt->active && held >= sp->clear_ms) {
                if (sp->latching) {
                    rt->latched = true;    /* keep active until Alarm_Acknowledge */
                } else {
                    rt->active = false;
                    log_edge(now, sp->code, sp->action, false);
                    if (tally->cleared++ == 0U) tally->first_cleared_desc = sp->desc;
                }
            }
        }
    }
}

/* One consolidated, rate-limited console line for a tick that had edges. */
static void log_tally(uint32_t now, const AlarmEdgeTally_t *t) {
    if ((t->raised == 0U) && (t->cleared == 0U)) return;
    if ((now - g_alarm.last_console_log_tick) < ALARM_LOG_MIN_INTERVAL_MS &&
        g_alarm.last_console_log_tick != 0U) {
        return;   /* suppressed -- event log ring already has the detail */
    }
    g_alarm.last_console_log_tick = now;
    LOG("ALARM: +%u -%u  %s%s\r\n",
        (unsigned)t->raised, (unsigned)t->cleared,
        t->first_raised_desc ? t->first_raised_desc
                             : (t->first_cleared_desc ? t->first_cleared_desc : ""),
        (t->raised + t->cleared > 1U) ? " (+more)" : "");
}

static void aggregate_view(void) {
    AlarmView_t v;
    memset(&v, 0, sizeof(v));
    v.highest_action = ALARM_ACT_INFO;
    v.worst_code = ALARM_NONE;

    for (uint8_t i = 0; i < ALARM_SPEC_COUNT; i++) {
        const AlarmSpec_t *sp = &k_specs[i];
        AlarmRt_t *rt = &g_alarm.rt[i];
        if (!rt->active) continue;

        v.active_mask |= (1ULL << sp->code);
        v.active_count++;
        if (rt->latched) v.latched_mask |= (1ULL << sp->code);

        if (sp->action > v.highest_action) {
            v.highest_action = sp->action;
            v.worst_code = sp->code;
        }
    }
    g_alarm.view = v;
}

static void dispatch_action(uint32_t now) {
    switch (g_alarm.view.highest_action) {
        case ALARM_ACT_ESTOP:
            if (!g_alarm.estop_sent) {
                g_alarm.estop_sent = true;
                g_alarm.stop_sent = true;
                LOG("ALARM: -> EMERGENCY STOP (code %u)\r\n", (unsigned)g_alarm.view.worst_code);
                ChargeController_EmergencyStop(now);
            }
            break;
        case ALARM_ACT_STOP:
            if (!g_alarm.stop_sent) {
                g_alarm.stop_sent = true;
                LOG("ALARM: -> STOP (code %u)\r\n", (unsigned)g_alarm.view.worst_code);
                ChargeController_Stop(now);
            }
            break;
        case ALARM_ACT_INFO:
        default:
            /* everything back to INFO/none -- re-arm the dispatch latches */
            g_alarm.stop_sent = false;
            g_alarm.estop_sent = false;
            break;
    }
}

/* ============== Public API ============== */

void Alarm_Init(void) {
    memset(&g_alarm, 0, sizeof(g_alarm));
    g_alarm.view.highest_action = ALARM_ACT_INFO;
    g_alarm.inited = true;
    LOG("ALARM: init (%u specs)\r\n", (unsigned)ALARM_SPEC_COUNT);
}

void Alarm_Process(uint32_t now_tick) {
    if (!g_alarm.inited) Alarm_Init();

    AlarmInputs_t in;
    gather_inputs(now_tick, &in);

    /* session trackers for DC_LOAD_LOST / DC_OUT_NOT_ESTABLISHED */
    if (in.cc.relay_should_close && !g_alarm.prev_relay_close) {
        g_alarm.relay_close_since = (now_tick == 0U) ? 1U : now_tick;
    }
    if (!in.cc.relay_should_close) {
        g_alarm.relay_close_since = 0U;
    }
    g_alarm.prev_relay_close = in.cc.relay_should_close;

    if (in.cc.state != CHARGE_CTRL_STATE_RUNNING) {
        g_alarm.load_established = false;
    } else if (in.mod_current_max > ALARM_I_LOAD_MIN_A) {
        g_alarm.load_established = true;
    }

    AlarmEdgeTally_t tally = {0};
    run_debounce(now_tick, &in, &tally);
    aggregate_view();
    dispatch_action(now_tick);
    log_tally(now_tick, &tally);
}

void Alarm_GetView(AlarmView_t *view) {
    if (view == NULL) return;
    *view = g_alarm.view;
}

void Alarm_Acknowledge(uint32_t now_tick) {
    AlarmInputs_t in;
    gather_inputs(now_tick, &in);

    uint8_t acked = 0;
    for (uint8_t i = 0; i < ALARM_SPEC_COUNT; i++) {
        const AlarmSpec_t *sp = &k_specs[i];
        AlarmRt_t *rt = &g_alarm.rt[i];
        if (!rt->latched) continue;
        if (sp->eval(&in, sp->param)) continue; /* still present */
        rt->latched = false;
        rt->active = false;
        rt->raw_prev = false;
        log_edge(now_tick, sp->code, sp->action, false);
        acked++;
    }
    if (acked > 0U) LOG("ALARM: %u latched cleared by ack\r\n", (unsigned)acked);
    aggregate_view();
    /* let dispatch_action re-arm on the next tick once nothing STOP-level remains */
}

uint8_t Alarm_GetLog(AlarmLogEntry_t *out, uint8_t max) {
    if (out == NULL || max == 0U) return 0U;
    uint8_t n = (g_alarm.log_count < max) ? g_alarm.log_count : max;
    for (uint8_t k = 0; k < n; k++) {
        /* newest-first: head-1 is newest */
        uint8_t idx = (uint8_t)((g_alarm.log_head + ALARM_LOG_DEPTH - 1U - k) % ALARM_LOG_DEPTH);
        out[k] = g_alarm.log[idx];
    }
    return n;
}
