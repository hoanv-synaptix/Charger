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
#include "bsp_adc.h"
#include "charge_cycle_storage.h"
#include "chg_lib.h"
#include "bms_core.h"
#include "debug_log.h"
#include "main.h"
#include <string.h>
#include <math.h>

/* Standalone completion policy: use fresh charger telemetry, not BMS data. */
#define CHARGE_CTRL_MODULE_VOLTAGE_MAX_AGE_MS       2000U
#define CHARGE_CTRL_STANDALONE_VMAX_CONFIRM_MS      1000U

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

static void set_fault(uint32_t flags);
static void clear_fault(void);
static void transition_to(ChargeCtrlState_t new_state);
static uint8_t get_active_module_count(void);
static bool check_preconditions_set_fault(void);
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

static void set_fault(uint32_t flags) {
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
        transition_to(CHARGE_CTRL_STATE_FAULT);
    }
}

static void clear_fault(void) {
    g_ctrl.fault_flags = CHARGE_CTRL_FAULT_NONE;
}

static void transition_to(ChargeCtrlState_t new_state) {
    if (g_ctrl.state == new_state) {
        return;
    }

    LOG("CC: State %d->%d\r\n", (int)g_ctrl.state, (int)new_state);
    g_ctrl.state = new_state;
    g_ctrl.last_update_tick = HAL_GetTick();
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
    if (count == 0 && total > 0) {
        /* Debug: why zero modules? */
        if (CHG_LIB_GetModuleView(0, &view)) {
            LOG("CC: ModCnt total=%u en=%u online=%u state=%d\r\n",
                (unsigned)total, (unsigned)view.enabled, (unsigned)view.online, (int)view.state);
        } else {
            LOG("CC: ModCnt total=%u (failed to get view 0)\r\n", (unsigned)total);
        }
    }
    return count;
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
static bool check_preconditions_set_fault(void) {
    uint32_t faults = check_preconditions_faults();
    if (faults != CHARGE_CTRL_FAULT_NONE) {
        set_fault(faults);
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
    transition_to(CHARGE_CTRL_STATE_STOPPING);
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
                set_fault(CHARGE_CTRL_FAULT_PROTECT_JACK_V);
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

static ChargeStageEval_t eval_cell_stage(const ChargeCycleConfig_t *cfg, const BMS_View_t *bms) {
    ChargeStageEval_t eval = {0};

    if (!cfg->cell_volt_enabled) {
        eval.enabled = 0;
        return eval;
    }

    eval.enabled = 1;
    eval.source = CHARGE_LIMIT_SOURCE_CELL_VOLTAGE;
    float cell_volt_v = (float)bms->max_cell_volt / 1000.0f;  /* mV -> V */

    /* Check thresholds to determine target band */
    ChargeStageBand_t new_band = CHARGE_STAGE_BAND_BELOW_MIN;
    if (cell_volt_v >= cfg->cell_volt_5_v) {
        new_band = CHARGE_STAGE_BAND_ABOVE_MAX;
    } else if (cell_volt_v >= cfg->cell_volt_4_v) {
        new_band = CHARGE_STAGE_BAND_4_5;
    } else if (cell_volt_v >= cfg->cell_volt_3_v) {
        new_band = CHARGE_STAGE_BAND_3_4;
    } else if (cell_volt_v >= cfg->cell_volt_2_v) {
        new_band = CHARGE_STAGE_BAND_2_3;
    } else if (cell_volt_v >= cfg->cell_volt_1_v) {
        new_band = CHARGE_STAGE_BAND_1_2;
    }

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

    /* Apply limits based on the evaluated band */
    switch (new_band) {
        case CHARGE_STAGE_BAND_BELOW_MIN:
            eval.state = CHARGE_STAGE_BELOW_MIN;
            eval.band = CHARGE_STAGE_BAND_BELOW_MIN;
            eval.inhibit = 1;
            eval.current_limit_c = 0.0f;
            break;
        case CHARGE_STAGE_BAND_1_2:
            eval.state = CHARGE_STAGE_IN_WINDOW;
            eval.band = CHARGE_STAGE_BAND_1_2;
            eval.current_limit_c = cfg->cell_curr_1_c;
            break;
        case CHARGE_STAGE_BAND_2_3:
            eval.state = CHARGE_STAGE_IN_WINDOW;
            eval.band = CHARGE_STAGE_BAND_2_3;
            eval.current_limit_c = cfg->cell_curr_2_c;
            break;
        case CHARGE_STAGE_BAND_3_4:
            eval.state = CHARGE_STAGE_IN_WINDOW;
            eval.band = CHARGE_STAGE_BAND_3_4;
            eval.current_limit_c = cfg->cell_curr_3_c;
            break;
        case CHARGE_STAGE_BAND_4_5:
            eval.state = CHARGE_STAGE_IN_WINDOW;
            eval.band = CHARGE_STAGE_BAND_4_5;
            eval.current_limit_c = cfg->cell_curr_4_c;
            break;
        default:
            eval.state = CHARGE_STAGE_ABOVE_MAX;
            eval.band = CHARGE_STAGE_BAND_ABOVE_MAX;
            eval.inhibit = 1;
            eval.current_limit_c = 0.0f;
            break;
    }

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

    /* Check thresholds to determine target band */
    ChargeStageBand_t new_band = CHARGE_STAGE_BAND_BELOW_MIN;
    if (temp_c >= cfg->temp_5_c) {
        new_band = CHARGE_STAGE_BAND_ABOVE_MAX;
    } else if (temp_c >= cfg->temp_4_c) {
        new_band = CHARGE_STAGE_BAND_4_5;
    } else if (temp_c >= cfg->temp_3_c) {
        new_band = CHARGE_STAGE_BAND_3_4;
    } else if (temp_c >= cfg->temp_2_c) {
        new_band = CHARGE_STAGE_BAND_2_3;
    } else if (temp_c >= cfg->temp_1_c) {
        new_band = CHARGE_STAGE_BAND_1_2;
    }

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

    /* Apply limits based on the evaluated band */
    switch (new_band) {
        case CHARGE_STAGE_BAND_BELOW_MIN:
            eval.state = CHARGE_STAGE_BELOW_MIN;
            eval.band = CHARGE_STAGE_BAND_BELOW_MIN;
            eval.inhibit = 1;
            eval.current_limit_c = 0.0f;
            break;
        case CHARGE_STAGE_BAND_1_2:
            eval.state = CHARGE_STAGE_IN_WINDOW;
            eval.band = CHARGE_STAGE_BAND_1_2;
            eval.current_limit_c = cfg->temp_curr_1_c;
            break;
        case CHARGE_STAGE_BAND_2_3:
            eval.state = CHARGE_STAGE_IN_WINDOW;
            eval.band = CHARGE_STAGE_BAND_2_3;
            eval.current_limit_c = cfg->temp_curr_2_c;
            break;
        case CHARGE_STAGE_BAND_3_4:
            eval.state = CHARGE_STAGE_IN_WINDOW;
            eval.band = CHARGE_STAGE_BAND_3_4;
            eval.current_limit_c = cfg->temp_curr_3_c;
            break;
        case CHARGE_STAGE_BAND_4_5:
            eval.state = CHARGE_STAGE_IN_WINDOW;
            eval.band = CHARGE_STAGE_BAND_4_5;
            eval.current_limit_c = cfg->temp_curr_4_c;
            break;
        default:
            eval.state = CHARGE_STAGE_ABOVE_MAX;
            eval.band = CHARGE_STAGE_BAND_ABOVE_MAX;
            eval.inhibit = 1;
            eval.current_limit_c = 0.0f;
            break;
    }

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

    /* Check thresholds to determine target band */
    ChargeStageBand_t new_band = CHARGE_STAGE_BAND_BELOW_MIN;
    if (soc_pct >= cfg->soc_5_pct) {
        new_band = CHARGE_STAGE_BAND_ABOVE_MAX;
    } else if (soc_pct >= cfg->soc_4_pct) {
        new_band = CHARGE_STAGE_BAND_4_5;
    } else if (soc_pct >= cfg->soc_3_pct) {
        new_band = CHARGE_STAGE_BAND_3_4;
    } else if (soc_pct >= cfg->soc_2_pct) {
        new_band = CHARGE_STAGE_BAND_2_3;
    } else if (soc_pct >= cfg->soc_1_pct) {
        new_band = CHARGE_STAGE_BAND_1_2;
    }

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

    /* Apply limits based on the evaluated band */
    switch (new_band) {
        case CHARGE_STAGE_BAND_BELOW_MIN:
            eval.state = CHARGE_STAGE_BELOW_MIN;
            eval.band = CHARGE_STAGE_BAND_BELOW_MIN;
            eval.inhibit = 1;
            eval.current_limit_c = 0.0f;
            break;
        case CHARGE_STAGE_BAND_1_2:
            eval.state = CHARGE_STAGE_IN_WINDOW;
            eval.band = CHARGE_STAGE_BAND_1_2;
            eval.current_limit_c = cfg->soc_curr_1_c;
            break;
        case CHARGE_STAGE_BAND_2_3:
            eval.state = CHARGE_STAGE_IN_WINDOW;
            eval.band = CHARGE_STAGE_BAND_2_3;
            eval.current_limit_c = cfg->soc_curr_2_c;
            break;
        case CHARGE_STAGE_BAND_3_4:
            eval.state = CHARGE_STAGE_IN_WINDOW;
            eval.band = CHARGE_STAGE_BAND_3_4;
            eval.current_limit_c = cfg->soc_curr_3_c;
            break;
        case CHARGE_STAGE_BAND_4_5:
            eval.state = CHARGE_STAGE_IN_WINDOW;
            eval.band = CHARGE_STAGE_BAND_4_5;
            eval.current_limit_c = cfg->soc_curr_4_c;
            break;
        default:
            eval.state = CHARGE_STAGE_ABOVE_MAX;
            eval.band = CHARGE_STAGE_BAND_ABOVE_MAX;
            eval.inhibit = 1;
            eval.current_limit_c = 0.0f;
            break;
    }

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
    /* ----- Jack temperature soft derating ----- */
    float mcu_adc_temp_c = -273.15f;
    for (uint8_t i = 0; i < 4; i++) {
        float temp = BSP_ADC_GetTempC(i);
        if (isfinite(temp) && temp > mcu_adc_temp_c) {
            mcu_adc_temp_c = temp;
        }
    }
    if (mcu_adc_temp_c < -50.0f) {
        mcu_adc_temp_c = 25.0f; // Fallback if all disconnected
    }

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
    LOG("CC: Manual V=%d.%dV I=%d.%dA/mod\r\n",
        v_int / 10, v_int % 10, i_int / 10, i_int % 10);

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
    LOG("CC: Standalone V=%d.%dV I=%d.%dA/mod\r\n",
        v_int / 10, v_int % 10, i_int / 10, i_int % 10);

    apply_charge_targets();
}

static void run_bms_controlled_mode(uint32_t now_tick) {
    (void)now_tick;

    ChargeCycleConfig_t cfg;
    ChargeCycleConfig_Get(&cfg);

    BMS_View_t bms;
    BMS_GetView(&bms);

    /* Check BMS conditions */
    if (!bms.online) {
        LOG("CC: BMS offline\r\n");
        set_fault(CHARGE_CTRL_FAULT_BMS_OFFLINE);
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

    /* Check critical BMS alarms */
    if (bms.alarm_flags & (BMS_ALARM_OVER_CHG_CURR | BMS_ALARM_HIGH_CELL_VOLT |
                           BMS_ALARM_TEMP_HIGH_CHG)) {
        LOG("CC: BMS alarm active\r\n");
        set_fault(CHARGE_CTRL_FAULT_BMS_ALARM);
        return;
    }

    /* Base targets: Ignore BMS requests entirely, use local configuration */
    g_ctrl.target_voltage_v = cfg.vmax_v;

    /* Clamp voltage to hardware limits */
    if (g_ctrl.target_voltage_v > cfg.module_u_max_v) {
        g_ctrl.target_voltage_v = cfg.module_u_max_v;
    }

    /* Compute stage limits */
    float stage_limit_c = 0.0f;
    uint8_t stage_inhibit = 0;
    uint8_t stage_limit_source = CHARGE_LIMIT_SOURCE_NONE;
    uint8_t stage_band = CHARGE_STAGE_BAND_NONE;
    compute_stage_limits(&cfg, &bms, &stage_limit_c, &stage_inhibit,
                         &stage_limit_source, &stage_band);

    g_ctrl.inhibit = stage_inhibit;
    g_ctrl.derating = 0;
    g_ctrl.active_limit_source = stage_limit_source;
    g_ctrl.active_stage_band = stage_band;
    g_ctrl.active_limit_current_c = stage_limit_c;

    /* Cell voltage and SOC completion end the current cycle.  Temperature
     * inhibit remains recoverable and is handled as a live block below. */
    if (g_ctrl.cell_full_latched || g_ctrl.soc_full_latched) {
        if (g_ctrl.cell_full_latched) {
            g_ctrl.stop_reason = CHARGE_STOP_CELL_VOLTAGE_REACHED;
        } else {
            g_ctrl.stop_reason = CHARGE_STOP_SOC_REACHED;
        }
        g_ctrl.target_current_total_a = 0.0f;
        g_ctrl.target_current_per_module_a = 0.0f;
        transition_to(CHARGE_CTRL_STATE_STOPPING);
        return;
    }

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
    LOG("CC: Init\r\n");
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
            set_fault(CHARGE_CTRL_FAULT_MODULE_COUNT_MISMATCH);
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
            if (check_preconditions_set_fault()) {
                transition_to(CHARGE_CTRL_STATE_RUNNING);
            } else {
                transition_to(CHARGE_CTRL_STATE_FAULT);
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
            transition_to(CHARGE_CTRL_STATE_IDLE);
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
}

bool ChargeController_CheckPreconditions(uint32_t *fault_flags_out) {
    uint32_t faults = check_preconditions_faults();
    if (fault_flags_out != NULL) {
        *fault_flags_out = faults;
    }
    return (faults == CHARGE_CTRL_FAULT_NONE);
}

bool ChargeController_Start(ChargeCtrlOwner_t owner, bool manual_mode) {
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
        transition_to(CHARGE_CTRL_STATE_FAULT);
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

    transition_to(CHARGE_CTRL_STATE_READY);
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

void ChargeController_Stop(void) {
    if (g_ctrl.state == CHARGE_CTRL_STATE_IDLE) {
        return;
    }

    LOG("CC: Stop requested\r\n");
    g_ctrl.stop_reason = CHARGE_STOP_USER_COMMAND;

    if (g_ctrl.state == CHARGE_CTRL_STATE_FAULT) {
        /* Clear fault and go to IDLE */
        clear_fault();
        transition_to(CHARGE_CTRL_STATE_IDLE);
        g_ctrl.owner = CHARGE_CTRL_OWNER_NONE;
    } else {
        transition_to(CHARGE_CTRL_STATE_STOPPING);
    }
}

void ChargeController_EmergencyStop(void) {
    LOG("CC: EMERGENCY STOP\r\n");

    /* Immediate hardware stop */
    CHG_LIB_EmergencyStop();

    /* Set fault flag */
    set_fault(CHARGE_CTRL_FAULT_EMERGENCY_STOP);

    /* Force to fault state */
    transition_to(CHARGE_CTRL_STATE_FAULT);

    g_ctrl.owner = CHARGE_CTRL_OWNER_NONE;
}

bool ChargeController_IsRunning(void) {
    return (g_ctrl.state == CHARGE_CTRL_STATE_RUNNING);
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
}




