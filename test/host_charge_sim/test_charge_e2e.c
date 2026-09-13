/**
 * @file test_charge_e2e.c
 * @brief Host-compiled end-to-end test: real BMS + charger-module CAN
 *        protocols (per docs/) -> real firmware logic (charge_controller.c
 *        / chg_lib_* / bms_core.c) -> the same getters the app sees
 *        (ChargeController_GetView / CHG_LIB_GetModuleView / BMS_GetView).
 *        See test/test_logic.c for the established ASSERT/bool-test-fn
 *        convention this file follows.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

#include "chg_lib.h"
#include "chg_lib_driver_maxwell.h"
#include "chg_lib_driver_lianming.h"
#include "chg_lib_driver_tonhe.h"
#include "chg_lib_can_backend.h"
#include "bms_core.h"
#include "charge_cycle_config.h"
#include "charge_controller.h"

#include "sim_can_modules.h"
#include "sim_bms.h"

/* Mock HAL used by charge_controller.c / bms_core.c / chg_lib_* (BSP_GetTick
 * etc. -- same pattern as test/test_logic.c). */
uint32_t mock_tick = 0;
uint32_t HAL_GetTick(void) { return mock_tick; }
uint32_t BSP_GetTick(void) { return mock_tick; }
void BSP_Delay(uint32_t delay_ms) { (void)delay_ms; }
void BSP_EnterCritical(void) {}
void BSP_ExitCritical(void) {}

/* Spy declared in test/mock_hal/mock_stubs.c's BSP_CAN_Transmit() -- lets a
 * test assert on transmitted CAN frame *content*, not just that a TX
 * happened. */
extern bool MockCan_GetLastTx(uint32_t ext_id, uint8_t data_out[8]);
#include "bms_protocol.h" /* BMS_ID_CTRL_INFO */

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            printf("[FAIL] %s:%d - %s\n", __func__, __LINE__, msg); \
            return false; \
        } \
    } while (0)

/* ================================================================== */
/* Driving loop -- mirrors App_Loop()'s call order (App/System/app_main.c):
 * CAN processing, then BMS, then the controller. */
/* ================================================================== */

static SimDriverKind_t g_active_kind;

static void drive_step(uint32_t step_ms)
{
    mock_tick += step_ms;
    sim_bms_tick(mock_tick);
    sim_module_tick(g_active_kind, mock_tick);
    CHG_LIB_Process(mock_tick);
    BMS_Process(mock_tick);
    ChargeController_Process(mock_tick);
}

static void drive_ms(uint32_t total_ms)
{
    for (uint32_t elapsed = 0; elapsed < total_ms; elapsed += 20U) {
        drive_step(20U);
    }
}

/* ================================================================== */
/* Scenario setup                                                      */
/* ================================================================== */

static SimDriverKind_t kind_for_module_type(uint8_t module_type)
{
    switch (module_type) {
        case CHARGE_MODULE_TYPE_MAXWELL:  return SIM_DRV_MAXWELL;
        case CHARGE_MODULE_TYPE_LIANMING: return SIM_DRV_LIANMING;
        default:                          return SIM_DRV_TONHE;
    }
}

static void build_default_cfg(ChargeCycleConfig_t *cfg, uint8_t module_type)
{
    ChargeCycleConfig_GetDefaults(cfg);
    cfg->module_type = module_type;
    cfg->source_module_count = 1;
    cfg->charge_source_mode = CHARGE_SOURCE_BMS_CONTROLLED;

    cfg->battery_capacity_ah = 100.0f;
    cfg->imin_c = 0.05f;
    cfg->imax_c = 1.0f;         /* 1C -> 100A max */
    cfg->vmin_v = 300.0f;
    cfg->vmax_v = 500.0f;

    cfg->module_u_min_v = 30.0f;
    cfg->module_u_max_v = 550.0f;
    cfg->module_i_min_a = 1.0f;
    cfg->module_i_max_a = 200.0f;

    /* Cell-voltage staging enabled and isolated (SOC/temp disabled) so
     * scenario 2 can validate band transitions deterministically. */
    cfg->cell_volt_enabled = 1;
    cfg->cell_volt_delta_t_s = 0.0f;
    cfg->soc_delta_t_s = 0.0f;
    cfg->cell_volt_1_v = 3.00f;
    cfg->cell_volt_2_v = 3.30f;
    cfg->cell_volt_3_v = 3.50f;
    cfg->cell_volt_4_v = 3.65f;
    cfg->cell_volt_5_v = 3.80f;
    cfg->cell_curr_1_c = 1.00f;
    cfg->cell_curr_2_c = 0.70f;
    cfg->cell_curr_3_c = 0.40f;
    cfg->cell_curr_4_c = 0.20f;
    cfg->temp_enabled = 0;
    cfg->soc_enabled = 0;
}

/* Resets every piece of global state the firmware modules own, then
 * registers+selects the given driver and adds one module -- mirrors
 * App_Init()'s CHG_LIB_RegisterDriver() calls (done once, guarded) plus
 * what ChargeCycleConfig_Set() does for module_type/source_module_count. */
static bool setup_scenario(uint8_t module_type, ChargeCycleConfig_t *cfg_out)
{
    static bool drivers_registered = false;
    if (!drivers_registered) {
        CHG_LIB_RegisterDriver(CHG_LIB_DRV_MAXWELL, CHG_LIB_MaxwellDriverOps());
        CHG_LIB_RegisterDriver(CHG_LIB_DRV_LIANMING, CHG_LIB_LianmingDriverOps());
        CHG_LIB_RegisterDriver(CHG_LIB_DRV_TONHE, CHG_LIB_TonheDriverOps());
        drivers_registered = true;
    }

    mock_tick = 0;
    g_active_kind = kind_for_module_type(module_type);
    sim_install_backend(g_active_kind);
    sim_module_reset(&g_sim_module, 1, 0);
    sim_bms_reset(&g_sim_bms);

    BMS_Init();
    ChargeCycleConfig_Init();
    ChargeController_Init();

    ChargeCycleConfig_t cfg;
    build_default_cfg(&cfg, module_type);
    if (!ChargeCycleConfig_Set(&cfg)) {
        printf("[FAIL] setup_scenario: ChargeCycleConfig_Set rejected config\n");
        return false;
    }
    if (cfg_out) *cfg_out = cfg;
    return true;
}

/* A realistic pack: cell voltages/temps in-range, nothing latched. Also
 * arms the module simulator to broadcast/answer as "present but idle". */
static void set_healthy_bms(float pack_v, uint8_t soc)
{
    g_sim_bms.pack_voltage_v = pack_v;
    g_sim_bms.pack_current_a = 0.0f;
    g_sim_bms.soc_pct = soc;
    g_sim_bms.max_cell_mv = 3000; /* sits in band 1_2 -> full current allowed */
    g_sim_bms.max_cv_no = 1;
    g_sim_bms.min_cell_mv = 2980;
    g_sim_bms.min_cv_no = 2;
    g_sim_bms.max_cell_temp_c = 25.0f;
    g_sim_bms.min_cell_temp_c = 24.0f;
    g_sim_bms.avg_cell_temp_c = 24.5f;
    g_sim_bms.cap_remain_x0_1ah = 500;
    g_sim_bms.rate_cap_x0_1ah = 1000; /* 100.0Ah */
    g_sim_bms.soh_pct = 100;
    g_sim_bms.chg_volt_request_v = 500.0f;
    g_sim_bms.chg_curr_request_a = 50.0f;
}

/* Warm up module + BMS to "online" and start the cycle, then drive until
 * either the module reports RUNNING or the timeout elapses. */
static bool warmup_and_start(uint32_t warmup_ms, uint32_t start_timeout_ms)
{
    drive_ms(warmup_ms);

    if (!ChargeController_Start(CHARGE_CTRL_OWNER_PC, false, mock_tick)) {
        printf("[FAIL] warmup_and_start: ChargeController_Start refused\n");
        return false;
    }

    for (uint32_t elapsed = 0; elapsed < start_timeout_ms; elapsed += 20U) {
        drive_step(20U);
        CHG_LIB_ModuleView_t mv;
        if (CHG_LIB_GetModuleView(0, &mv) && mv.state == CHG_LIB_STATE_RUNNING) {
            return true;
        }
    }
    return false;
}

/* ================================================================== */
/* Scenario 1/2/3 per driver: happy path, stage/derating, module fault */
/* ================================================================== */

static bool test_driver_happy_path(uint8_t module_type, const char *name)
{
    printf("Running test_%s_happy_path...\n", name);
    ChargeCycleConfig_t cfg;
    ASSERT(setup_scenario(module_type, &cfg), "setup failed");
    set_healthy_bms(400.0f, 50);

    bool reached_running = warmup_and_start(1500U, 4000U);
    ASSERT(reached_running, "module never reached RUNNING");

    CHG_LIB_ModuleView_t mv;
    ASSERT(CHG_LIB_GetModuleView(0, &mv), "module view unavailable");
    ASSERT(mv.online, "module should be online");
    ASSERT(mv.running, "module should be running");
    ASSERT(mv.alarm_flags == CHG_LIB_ALARM_NONE, "unexpected alarm on happy path");

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_RUNNING, "controller should be RUNNING");
    ASSERT(cv.target_voltage_v > 0.0f, "target voltage should be set");
    /* BUGFIX 2026-08-29: target_voltage_v is Stage-1 (BMS pack voltage,
     * 400V from set_healthy_bms() above) until the relay latches closed --
     * this scenario never ramps module voltage to arm it, so it stays at
     * Stage-1, not cfg.vmax_v (500V). See update_relay_decision()'s
     * docstring and the target_voltage_v assignment in
     * run_bms_controlled_mode(). Still not derived from the BMS's own
     * ChgRequest_INFO (bms.chg_volt_request) -- that's a separate,
     * still-true guarantee (SRS TBD-04). */
    ASSERT(fabsf(cv.target_voltage_v - 400.0f) < 0.01f,
           "target voltage should be Stage-1 (BMS pack voltage) before the relay arms, not vmax_v");

    BMS_View_t bv;
    BMS_GetView(&bv);
    ASSERT(bv.online, "BMS should be online");
    ASSERT(bv.state == BMS_STATE_ONLINE, "BMS state should be ONLINE");

    printf("[PASS] test_%s_happy_path\n", name);
    return true;
}

static bool test_driver_stage_derating(uint8_t module_type, const char *name)
{
    printf("Running test_%s_stage_derating...\n", name);
    ChargeCycleConfig_t cfg;
    ASSERT(setup_scenario(module_type, &cfg), "setup failed");
    set_healthy_bms(400.0f, 50);
    g_sim_bms.max_cell_mv = 2800; /* below cell_volt_1_v(3.00V) -> BELOW_MIN, inhibited */

    /* BELOW_MIN inhibits current entirely, so should_run in
     * apply_charge_targets() stays false and the module never gets a START
     * command -- warmup_and_start()'s "reached RUNNING" wait is expected to
     * time out here. Drive it anyway (it still performs the warmup +
     * Start()) and check the *controller's* inhibit/target instead of
     * module RUNNING. */
    (void)warmup_and_start(1500U, 4000U);
    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.inhibit == 1, "should be inhibited below cell_volt_1_v");
    ASSERT(cv.active_limit_source == CHARGE_LIMIT_SOURCE_CELL_VOLTAGE, "limit source should be cell voltage");

    /* Ramp into band 1_2 -> current allowed at cell_curr_1_c (1.0C = 100A) */
    g_sim_bms.max_cell_mv = 3100;
    drive_ms(600U);
    ChargeController_GetView(&cv);
    ASSERT(cv.inhibit == 0, "should no longer be inhibited in band 1_2");
    ASSERT(cv.active_stage_band == CHARGE_STAGE_BAND_1_2, "should be in stage band 1_2");
    ASSERT(fabsf(cv.target_current_total_a - 100.0f) < 1.0f, "band 1_2 should allow full 1.0C (100A)");

    /* Ramp into band 3_4 (cell_curr_3_c = 0.4C = 40A) */
    g_sim_bms.max_cell_mv = 3550;
    drive_ms(600U);
    ChargeController_GetView(&cv);
    ASSERT(cv.active_stage_band == CHARGE_STAGE_BAND_3_4, "should be in stage band 3_4");
    ASSERT(fabsf(cv.target_current_total_a - 40.0f) < 1.0f, "band 3_4 should derate to 0.4C (40A)");
    ASSERT(cv.derating == 1, "derating flag should be set once current is limited below imax_c");

    printf("[PASS] test_%s_stage_derating\n", name);
    return true;
}

static bool test_driver_module_fault(uint8_t module_type, const char *name)
{
    printf("Running test_%s_module_fault...\n", name);
    ASSERT(setup_scenario(module_type, NULL), "setup failed");
    set_healthy_bms(400.0f, 50);
    ASSERT(warmup_and_start(1500U, 4000U), "module never reached RUNNING");

    /* Inject a real documented fault bit per driver and let it propagate. */
    switch (module_type) {
        case CHARGE_MODULE_TYPE_MAXWELL:
            g_sim_module.maxwell_alarm_raw = (1U << 28); /* MXR_ALARM_SHORT_CIRCUIT */
            break;
        case CHARGE_MODULE_TYPE_LIANMING:
            g_sim_module.lianming_status_raw = (1U << 4); /* input overvoltage */
            break;
        default:
            g_sim_module.tonhe_fault_bits = (1U << 5); /* over-temperature */
            break;
    }
    drive_ms(1200U);

    CHG_LIB_ModuleView_t mv;
    ASSERT(CHG_LIB_GetModuleView(0, &mv), "module view unavailable");
    ASSERT(mv.state == CHG_LIB_STATE_FAULT, "module should be in FAULT after injected alarm bit");
    ASSERT(mv.alarm_flags != CHG_LIB_ALARM_NONE, "alarm_flags should be decoded from the raw bit");

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    /* The controller only watches BMS alarms directly; a module fault stops
     * charging via CHG_LIB module state (module no longer counted active),
     * which the next Start()/precondition check would catch. Assert what
     * the app would actually see: module view reporting the fault. */
    (void)cv;

    printf("[PASS] test_%s_module_fault\n", name);
    return true;
}

/* Regression test for B-08 (reclassified): FAULT recovery used to clear on
 * a single clean read -- weaker confirmation than OFFLINE->RECOVERING's
 * 5-consecutive-read debounce for a mere comm gap, backwards for a real
 * hardware fault. Confirms the module stays FAULT through the first few
 * clean reads after the fault condition clears, and only leaves FAULT
 * once 5 have accumulated. */
static bool test_driver_fault_recovery_debounce(uint8_t module_type, const char *name)
{
    printf("Running test_%s_fault_recovery_debounce...\n", name);
    ASSERT(setup_scenario(module_type, NULL), "setup failed");
    set_healthy_bms(400.0f, 50);
    ASSERT(warmup_and_start(1500U, 4000U), "module never reached RUNNING");

    switch (module_type) {
        case CHARGE_MODULE_TYPE_MAXWELL:
            g_sim_module.maxwell_alarm_raw = (1U << 28); /* MXR_ALARM_SHORT_CIRCUIT */
            break;
        case CHARGE_MODULE_TYPE_LIANMING:
            g_sim_module.lianming_status_raw = (1U << 4); /* input overvoltage */
            break;
        default:
            g_sim_module.tonhe_fault_bits = (1U << 5); /* over-temperature */
            break;
    }
    drive_ms(1200U);

    CHG_LIB_ModuleView_t mv;
    ASSERT(CHG_LIB_GetModuleView(0, &mv), "module view unavailable");
    ASSERT(mv.state == CHG_LIB_STATE_FAULT, "module should be in FAULT after injected alarm bit");

    /* Clear the fault condition in the simulator, then check just after
     * the first couple of clean reads: must still be FAULT, not yet
     * trusted as recovered. */
    g_sim_module.maxwell_alarm_raw = 0;
    g_sim_module.lianming_status_raw = 0;
    g_sim_module.tonhe_fault_bits = 0;
    g_sim_module.tonhe_pfc_bits = 0;
    drive_ms(60U); /* ~2-3 read/broadcast cycles at the 20ms drive step */

    ASSERT(CHG_LIB_GetModuleView(0, &mv), "module view unavailable");
    ASSERT(mv.state == CHG_LIB_STATE_FAULT,
           "must NOT recover after only 1-2 clean reads -- that's the pre-fix bug");

    /* Drive enough further time to accumulate 5+ clean reads and confirm
     * it does eventually recover. */
    drive_ms(1000U);
    ASSERT(CHG_LIB_GetModuleView(0, &mv), "module view unavailable");
    ASSERT(mv.state != CHG_LIB_STATE_FAULT,
           "should recover once 5 clean reads have accumulated since FAULT entry");
    ASSERT(mv.alarm_flags == CHG_LIB_ALARM_NONE, "alarm_flags should be clear after recovery");

    printf("[PASS] test_%s_fault_recovery_debounce\n", name);
    return true;
}

static bool test_distinct_module_alarm(uint8_t module_type,
                                       uint32_t maxwell_raw,
                                       uint16_t lianming_raw,
                                       uint16_t tonhe_raw,
                                       CHG_LIB_AlarmFlag_t expected,
                                       CHG_LIB_AlarmFlag_t forbidden,
                                       const char *label)
{
    ASSERT(setup_scenario(module_type, NULL), "setup failed");
    set_healthy_bms(400.0f, 50);
    ASSERT(warmup_and_start(1500U, 4000U), "module never reached RUNNING");

    g_sim_module.maxwell_alarm_raw = maxwell_raw;
    g_sim_module.lianming_status_raw = lianming_raw;
    g_sim_module.tonhe_fault_bits = tonhe_raw;
    drive_ms(800U);

    CHG_LIB_ModuleView_t mv;
    ASSERT(CHG_LIB_GetModuleView(0, &mv), "module view unavailable");
    if ((mv.alarm_flags & expected) == 0U) {
        printf("[FAIL] %s: expected flag 0x%08lX, got 0x%08lX\n",
               label, (unsigned long)expected, (unsigned long)mv.alarm_flags);
        return false;
    }
    if ((mv.alarm_flags & forbidden) != 0U) {
        printf("[FAIL] %s: incorrectly collapsed into 0x%08lX\n",
               label, (unsigned long)forbidden);
        return false;
    }
    ASSERT(mv.state == CHG_LIB_STATE_FAULT, "specific module alarm should enter FAULT");
    return true;
}

static bool test_module_warning(uint8_t module_type,
                                uint32_t maxwell_raw,
                                uint16_t lianming_raw,
                                uint16_t tonhe_raw,
                                CHG_LIB_AlarmFlag_t expected,
                                CHG_LIB_AlarmFlag_t forbidden,
                                const char *label)
{
    ASSERT(setup_scenario(module_type, NULL), "setup failed");
    set_healthy_bms(400.0f, 50);
    ASSERT(warmup_and_start(1500U, 4000U), "module never reached RUNNING");

    g_sim_module.maxwell_alarm_raw = maxwell_raw;
    g_sim_module.lianming_status_raw = lianming_raw;
    g_sim_module.tonhe_fault_bits = tonhe_raw;
    drive_ms(800U);

    CHG_LIB_ModuleView_t mv;
    ASSERT(CHG_LIB_GetModuleView(0, &mv), "module view unavailable");
    ASSERT((mv.alarm_flags & expected) != 0U, label);
    ASSERT((mv.alarm_flags & forbidden) == 0U, "warning collapsed into another alarm");
    ASSERT(mv.state != CHG_LIB_STATE_FAULT, "warning must not force module FAULT");
    return true;
}

static bool test_module_raw_bits_ignored(uint8_t module_type,
                                         uint16_t lianming_raw,
                                         const char *label)
{
    ASSERT(setup_scenario(module_type, NULL), "setup failed");
    set_healthy_bms(400.0f, 50);
    ASSERT(warmup_and_start(1500U, 4000U), "module never reached RUNNING");
    g_sim_module.lianming_status_raw = lianming_raw;
    drive_ms(800U);

    CHG_LIB_ModuleView_t mv;
    ASSERT(CHG_LIB_GetModuleView(0, &mv), "module view unavailable");
    ASSERT(mv.alarm_flags == CHG_LIB_ALARM_NONE, label);
    return true;
}

static bool test_tonhe_pfc_flag(uint8_t pfc_bits, const char *label)
{
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_TONHE, NULL), "setup failed");
    set_healthy_bms(400.0f, 50);
    ASSERT(warmup_and_start(1500U, 4000U), "module never reached RUNNING");
    g_sim_module.tonhe_pfc_bits = pfc_bits;
    drive_ms(800U);

    CHG_LIB_ModuleView_t mv;
    ASSERT(CHG_LIB_GetModuleView(0, &mv), "module view unavailable");
    ASSERT((mv.alarm_flags & CHG_LIB_ALARM_PFC_FAULT) != 0U, label);
    return true;
}

static bool test_distinct_module_alarm_matrix(void)
{
    printf("Running test_distinct_module_alarm_matrix...\n");
    ASSERT(test_distinct_module_alarm(CHARGE_MODULE_TYPE_MAXWELL, (1U << 27), 0, 0,
                                      CHG_LIB_ALARM_FAN_FAULT, CHG_LIB_ALARM_HW_FAULT,
                                      "Maxwell fan fault"), "Maxwell fan test failed");
    ASSERT(test_distinct_module_alarm(CHARGE_MODULE_TYPE_MAXWELL, (1U << 9), 0, 0,
                                      CHG_LIB_ALARM_AC_OVER_VOLT, CHG_LIB_ALARM_OVER_VOLTAGE_OUT,
                                      "Maxwell AC input overvoltage"), "Maxwell AC OV test failed");
    ASSERT(test_distinct_module_alarm(CHARGE_MODULE_TYPE_MAXWELL, (1U << 8), 0, 0,
                                      CHG_LIB_ALARM_PFC_FAULT, CHG_LIB_ALARM_HW_FAULT,
                                      "Maxwell PFC abnormal"), "Maxwell PFC test failed");
    ASSERT(test_distinct_module_alarm(CHARGE_MODULE_TYPE_MAXWELL, (1U << 4) | (1U << 5), 0, 0,
                                      CHG_LIB_ALARM_HW_FAULT, CHG_LIB_ALARM_AC_PHASE_LOSS,
                                      "Maxwell input mode/wiring fault"), "Maxwell input fault test failed");
    ASSERT(test_distinct_module_alarm(CHARGE_MODULE_TYPE_MAXWELL, (1U << 7), 0, 0,
                                      CHG_LIB_ALARM_OVER_VOLTAGE_OUT, CHG_LIB_ALARM_OUTPUT_OVER_VOLT_WARN,
                                      "Maxwell output OV protection"), "Maxwell output OV test failed");
    ASSERT(test_distinct_module_alarm(CHARGE_MODULE_TYPE_MAXWELL, (1U << 28), 0, 0,
                                      CHG_LIB_ALARM_SHORT_CIRCUIT, CHG_LIB_ALARM_HW_FAULT,
                                      "Maxwell short circuit"), "Maxwell short test failed");
    ASSERT(test_distinct_module_alarm(CHARGE_MODULE_TYPE_LIANMING, 0, (1U << 3), 0,
                                      CHG_LIB_ALARM_FAN_FAULT, CHG_LIB_ALARM_HW_FAULT,
                                      "Lianming fan fault"), "Lianming fan test failed");
    ASSERT(test_distinct_module_alarm(CHARGE_MODULE_TYPE_LIANMING, 0, (1U << 4), 0,
                                      CHG_LIB_ALARM_AC_OVER_VOLT, CHG_LIB_ALARM_HW_FAULT,
                                      "Lianming AC input overvoltage"), "Lianming AC OV test failed");
    ASSERT(test_module_warning(CHARGE_MODULE_TYPE_LIANMING, 0, (1U << 7), 0,
                               CHG_LIB_ALARM_OUTPUT_UNDER_VOLT, CHG_LIB_ALARM_HW_FAULT,
                               "Lianming output undervoltage warning"), "Lianming output UV test failed");
    ASSERT(test_module_raw_bits_ignored(CHARGE_MODULE_TYPE_LIANMING, (1U << 13) | (1U << 14),
                                        "Lianming raw bits 13/14 must be ignored"),
           "Lianming raw bit compatibility test failed");
    ASSERT(test_distinct_module_alarm(CHARGE_MODULE_TYPE_TONHE, 0, 0, (1U << 6),
                                      CHG_LIB_ALARM_FAN_FAULT, CHG_LIB_ALARM_HW_FAULT,
                                      "TonHe fan fault"), "TonHe fan test failed");
    ASSERT(test_distinct_module_alarm(CHARGE_MODULE_TYPE_TONHE, 0, 0, (1U << 2),
                                      CHG_LIB_ALARM_AC_OVER_VOLT, CHG_LIB_ALARM_OVER_VOLTAGE_OUT,
                                      "TonHe AC input overvoltage"), "TonHe AC OV test failed");
    ASSERT(test_distinct_module_alarm(CHARGE_MODULE_TYPE_TONHE, 0, 0, (1U << 3),
                                      CHG_LIB_ALARM_OVER_VOLTAGE_OUT, CHG_LIB_ALARM_OUTPUT_OVER_VOLT_WARN,
                                      "TonHe output OV protection"), "TonHe output OV test failed");
    ASSERT(test_module_warning(CHARGE_MODULE_TYPE_TONHE, 0, 0, (1U << 12),
                               CHG_LIB_ALARM_OUTPUT_UNDER_VOLT, CHG_LIB_ALARM_OVER_VOLTAGE_OUT,
                               "TonHe output undervoltage warning"), "TonHe output UV test failed");
    ASSERT(test_module_warning(CHARGE_MODULE_TYPE_TONHE, 0, 0, (1U << 13),
                               CHG_LIB_ALARM_OUTPUT_OVER_VOLT_WARN, CHG_LIB_ALARM_OVER_VOLTAGE_OUT,
                               "TonHe output overvoltage warning"), "TonHe output OV warning test failed");
    ASSERT(test_distinct_module_alarm(CHARGE_MODULE_TYPE_TONHE, 0, 0, (1U << 1),
                                      CHG_LIB_ALARM_AC_PHASE_LOSS, CHG_LIB_ALARM_HW_FAULT,
                                      "TonHe phase loss"), "TonHe phase-loss test failed");
    ASSERT(test_tonhe_pfc_flag((1U << 6), "TonHe PFC phase exception must be normalized"),
           "TonHe PFC phase-loss test failed");
    printf("[PASS] test_distinct_module_alarm_matrix\n");
    return true;
}

/* ================================================================== */
/* Relay decision (ChargeCtrlView_t.relay_should_close) -- see           */
/* update_relay_decision() in App/Charge/charge_controller.c             */
/* ================================================================== */

static bool test_relay_bms_mode(void)
{
    printf("Running test_relay_bms_mode...\n");
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_TONHE, NULL), "setup failed");
    set_healthy_bms(400.0f, 50);
    ASSERT(warmup_and_start(1500U, 4000U), "module never reached RUNNING");

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.target_voltage_v > 0.0f, "target voltage should be set");

    /* BUGFIX 2026-08-29: the 90% arming threshold now compares module
     * voltage against the BMS's real pack voltage (BmsView.batt_voltage),
     * not the final charge target -- see update_relay_decision()'s
     * docstring. set_healthy_bms(400.0f, ...) above means the reference
     * here is 400V, independent of whatever target_voltage_v is. */
    const float bms_ref = 400.0f;

    /* Below 90% of BMS pack voltage: relay must stay open even though the
     * controller is RUNNING and the BMS is otherwise healthy. */
    g_sim_module.voltage = bms_ref * 0.80f;
    drive_ms(200U);
    ChargeController_GetView(&cv);
    ASSERT(cv.relay_should_close == 0, "relay must stay open below 90% of BMS pack voltage");
    /* BUGFIX 2026-08-29: while not yet latched, the module must be
     * *commanded* toward the BMS pack voltage (Stage-1), not the far final
     * target (500V, per build_default_cfg's vmax_v) -- see the
     * target_voltage_v assignment in run_bms_controlled_mode(). */
    ASSERT(fabsf(cv.target_voltage_v - bms_ref) < 0.01f,
           "target_voltage_v must be Stage-1 (BMS pack voltage), not the far final target, before the relay arms");

    /* >=90% of BMS pack voltage, BMS healthy (bms_relay_allow default true
     * from sim_bms_reset): relay closes (arms the latch). */
    g_sim_module.voltage = bms_ref * 0.97f;
    drive_ms(200U);
    ChargeController_GetView(&cv);
    ASSERT(cv.relay_should_close == 1, "relay should close once voltage >=90% of BMS pack voltage and BMS reports safe");
    /* One more tick so run_bms_controlled_mode() re-evaluates
     * target_voltage_v against the now-latched relay (1-tick lag by
     * design -- see run_bms_controlled_mode()'s comment). */
    drive_ms(20U);
    ChargeController_GetView(&cv);
    ASSERT(fabsf(cv.target_voltage_v - 500.0f) < 0.01f,
           "target_voltage_v must jump to the real final target (Stage-2) once the relay has latched closed");

    /* Voltage sags back below 90% (e.g. CV-phase current taper) -- this is
     * explicitly NOT a fault once latched, relay must stay closed. */
    g_sim_module.voltage = bms_ref * 0.70f;
    drive_ms(200U);
    ChargeController_GetView(&cv);
    ASSERT(cv.relay_should_close == 1, "relay must stay closed when voltage sags after latching -- not a re-open trigger");
    ASSERT(cv.state == CHARGE_CTRL_STATE_RUNNING, "still RUNNING, this is normal charging behaviour");

    /* BUGFIX 2026-08-29: BMS's own relay-allow flag (BmsSwSta charge_sta,
     * mirrored here by g_sim_bms.bms_relay_allow) is no longer a gating
     * condition -- BMS_ShouldCloseChargeRelay() dropped this check
     * (confirmed on real hardware: this BMS unit never closes its own
     * relay in response to our Ctrl_INFO request, so requiring it would
     * permanently block ours). Flipping it false must have NO effect on
     * our relay now. */
    g_sim_bms.bms_relay_allow = false;
    drive_ms(700U); /* BmsSwSta resend interval is 500ms */
    ChargeController_GetView(&cv);
    ASSERT(cv.relay_should_close == 1, "BMS's own relay-allow flag must no longer gate our relay -- we don't control it in practice");
    ASSERT(cv.state == CHARGE_CTRL_STATE_RUNNING, "a false BMS relay-allow flag alone must not fault the whole cycle");

    printf("[PASS] test_relay_bms_mode\n");
    return true;
}

/* Shared setup for the current-gated-open tests below: get to RUNNING with
 * the relay latched closed, same recipe as test_relay_bms_mode's happy
 * path. Returns the BMS pack voltage used (bms_ref) so callers can reuse
 * it if needed; asserts internally via the caller's ASSERT macro context
 * is not possible from a helper, so callers must check the return. */
static bool setup_relay_closed_bms_mode(void)
{
    if (!setup_scenario(CHARGE_MODULE_TYPE_TONHE, NULL)) return false;
    set_healthy_bms(400.0f, 50);
    if (!warmup_and_start(1500U, 4000U)) return false;
    g_sim_module.voltage = 400.0f * 0.97f; /* >=95% arm threshold of the 400V pack */
    drive_ms(200U);
    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    return cv.relay_should_close == 1;
}

/* Regression test: relay must not open the instant a fault/stop is seen
 * while latched closed -- breaking a DC relay under real charging current
 * risks arcing/contact welding (no natural current zero-crossing, unlike
 * AC). User-confirmed 2026-08-29: wait for the module's own CAN-reported
 * current to settle near zero before actually opening. */
static bool test_relay_stays_closed_until_current_settles(void)
{
    printf("Running test_relay_stays_closed_until_current_settles...\n");
    ASSERT(setup_relay_closed_bms_mode(), "relay never latched closed in setup");

    g_sim_module.current_override = true; /* stop the simulator auto-zeroing current on Stop */
    g_sim_module.current = 20.0f; /* still charging hard */
    ChargeController_Stop(mock_tick);
    drive_ms(200U);

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_STOPPING,
           "controller must remain STOPPING until relay current settles");
    ASSERT(cv.relay_should_close == 1, "relay must stay closed while module current is still high (20A)");

    g_sim_module.current = 0.5f; /* below RELAY_OPEN_CURRENT_THRESHOLD_A (1.0A) */
    drive_ms(60U); /* a couple ticks for the fresh reading to be picked up */
    ChargeController_GetView(&cv);
    ASSERT(cv.relay_should_close == 0, "relay must open once module current has settled near zero");
    ASSERT(cv.state == CHARGE_CTRL_STATE_IDLE,
           "controller may become IDLE only after the relay has opened");

    printf("[PASS] test_relay_stays_closed_until_current_settles\n");
    return true;
}

/* Regression test: if the module's current reading never settles (e.g.
 * comms dropped mid-ramp-down, or a stuck sensor), the relay must not be
 * wedged closed forever -- RELAY_OPEN_TIMEOUT_MS bounds the wait. */
static bool test_relay_opens_on_timeout_if_current_never_settles(void)
{
    printf("Running test_relay_opens_on_timeout_if_current_never_settles...\n");
    ASSERT(setup_relay_closed_bms_mode(), "relay never latched closed in setup");

    g_sim_module.current_override = true; /* stop the simulator auto-zeroing current on Stop */
    g_sim_module.current = 20.0f;
    ChargeController_Stop(mock_tick);
    drive_ms(200U);

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_STOPPING,
           "controller must remain STOPPING while waiting for relay-open timeout");
    ASSERT(cv.relay_should_close == 1, "relay must still be closed right after Stop (current still high)");

    /* Current deliberately never drops -- only the timeout should open it. */
    drive_ms(2600U); /* short of RELAY_OPEN_TIMEOUT_MS (3000ms) from the Stop */
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_STOPPING,
           "controller must remain STOPPING before relay-open timeout");
    ASSERT(cv.relay_should_close == 1, "relay must still be closed just before the timeout elapses");

    drive_ms(600U); /* now past 3000ms total since Stop */
    ChargeController_GetView(&cv);
    ASSERT(cv.relay_should_close == 0, "relay must open once RELAY_OPEN_TIMEOUT_MS elapses, even with current still high");
    ASSERT(cv.state == CHARGE_CTRL_STATE_IDLE,
           "controller may become IDLE only after the relay-open timeout opens it");

    printf("[PASS] test_relay_opens_on_timeout_if_current_never_settles\n");
    return true;
}

/* Regression test: EMERGENCY_STOP is the one exception to the
 * current-gated-open wait -- user-confirmed 2026-08-29 ("lúc đó là khẩn
 * cấp rồi"): open immediately regardless of current, speed over arc risk. */
static bool test_relay_opens_immediately_on_emergency_stop(void)
{
    printf("Running test_relay_opens_immediately_on_emergency_stop...\n");
    ASSERT(setup_relay_closed_bms_mode(), "relay never latched closed in setup");

    g_sim_module.current_override = true; /* stop the simulator auto-zeroing current on Stop */
    g_sim_module.current = 20.0f; /* still charging hard */
    ChargeController_EmergencyStop(mock_tick);
    drive_ms(20U); /* a single tick is enough -- must not wait */

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_FAULT, "controller should be in FAULT after EmergencyStop");
    ASSERT(cv.relay_should_close == 0, "EMERGENCY_STOP must open the relay immediately, even with current still high");

    printf("[PASS] test_relay_opens_immediately_on_emergency_stop\n");
    return true;
}

/* Regression test: bms_critical_alarm_mask() (Modules/bms/bms_core.c)
 * includes BMS_ALARM_HIGH_PACK_VOLT and BMS_ALARM_TEMP_LOW_CHG, both of
 * which therefore already fail BMS_ShouldCloseChargeRelay() -- but
 * run_bms_controlled_mode()'s own critical-alarm check used to omit both,
 * so the controller would stay RUNNING (module still actively sourcing
 * current) while the relay independently wanted to open. Fixed by
 * matching the two masks. */
static bool test_bms_high_pack_volt_alarm_stops_module(void)
{
    printf("Running test_bms_high_pack_volt_alarm_stops_module...\n");
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_TONHE, NULL), "setup failed");
    set_healthy_bms(400.0f, 50);
    ASSERT(warmup_and_start(1500U, 4000U), "module never reached RUNNING");

    g_sim_bms.high_pack_volt = 3; /* severe -- only >=2 counts per map_alarm_field() */
    drive_ms(1000U);

    BMS_View_t bv;
    BMS_GetView(&bv);
    ASSERT(bv.alarm_flags & BMS_ALARM_HIGH_PACK_VOLT, "BMS_ALARM_HIGH_PACK_VOLT should be set");

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_FAULT, "controller should FAULT on HIGH_PACK_VOLT now that the mask matches bms_critical_alarm_mask()");
    ASSERT(cv.stop_reason == CHARGE_STOP_BMS_ALARM, "stop reason should be BMS_ALARM");

    printf("[PASS] test_bms_high_pack_volt_alarm_stops_module\n");
    return true;
}

/* Regression test for B-24 (docs/AUDIT_Findings.md sec 5.2): a deeply
 * discharged pack has target_voltage_v (500V, the *final* charge setpoint)
 * far above the pack's actual current voltage (300V, from the BMS). The
 * module realistically only gets partway there (280V) before this check
 * runs -- under the old target-relative threshold (90% of 500V = 450V)
 * the relay would never arm; under the new BMS-relative threshold (90% of
 * 300V = 270V) it must, since 280V clears it. Also proves the module
 * voltage no longer needs to overshoot anywhere near the final target
 * before the relay can close (the actual root of B-24's suspected
 * chicken-and-egg deadlock: some modules won't ramp output voltage at all
 * while genuinely unloaded, so target-relative 90% could be unreachable
 * before the relay -- which is the only source of a real load -- ever
 * closes). */
static bool test_relay_arms_off_bms_voltage_not_target(void)
{
    printf("Running test_relay_arms_off_bms_voltage_not_target...\n");
    ChargeCycleConfig_t cfg;
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_TONHE, &cfg), "setup failed");
    set_healthy_bms(300.0f, 20); /* deeply discharged pack, 300V */
    ASSERT(warmup_and_start(1500U, 4000U), "module never reached RUNNING");

    /* Sanity on the fixture: the config's *final* target is far above the
     * pack's current voltage. Check cfg.vmax_v directly, not
     * cv.target_voltage_v -- the latter is now Stage-1 (pack voltage)
     * until the relay arms, by design (see update_relay_decision()'s
     * docstring), so it would read ~300V here, not the final target. */
    ASSERT(cfg.vmax_v > 350.0f, "target should be well above the pack's current voltage (sanity on the fixture)");

    g_sim_module.voltage = 290.0f; /* >=95% arm threshold of the 300V pack, but nowhere near 95% of the ~500V target */
    drive_ms(200U);
    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.relay_should_close == 1, "relay must close off 90% of BMS pack voltage even though module is far below 90% of target_voltage_v");

    printf("[PASS] test_relay_arms_off_bms_voltage_not_target\n");
    return true;
}

/* Regression test: BMS_SendCtrlInfo() (Modules/bms/bms_core.c) existed to
 * set g_charge_ctrl.allow_charge but had no caller anywhere in the
 * firmware -- Ctrl_INFO kept transmitting every 500ms on schedule
 * (FR-BMS-06) but with chg_sw always 0, even while actively RUNNING. A
 * real BMS gating its own internal charge relay on that byte would never
 * report charge_relay_closed=true, blocking update_relay_decision()'s
 * BMS_ShouldCloseChargeRelay() check regardless of module voltage.
 * Confirmed via real HIL run 2026-08-29: module voltage already >90% of
 * target, relay still never closed. Fixed by charge_controller.c's new
 * update_bms_charge_allow(), called every tick alongside
 * update_relay_decision(): sends allow_charge=true only while state is
 * RUNNING (user-confirmed choice), false otherwise. */
static bool test_bms_ctrl_info_allow_charge_wired(void)
{
    printf("Running test_bms_ctrl_info_allow_charge_wired...\n");
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_TONHE, NULL), "setup failed");
    set_healthy_bms(400.0f, 50);

    uint8_t data[8];
    ASSERT(warmup_and_start(1500U, 4000U), "module never reached RUNNING");
    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_RUNNING, "controller should be RUNNING by now");
    /* update_bms_charge_allow() is edge-triggered -- the very next
     * ChargeController_Process() tick after entering RUNNING sends it. */
    drive_ms(40U);
    ASSERT(MockCan_GetLastTx(BMS_ID_CTRL_INFO, data), "Ctrl_INFO must have been sent");
    ASSERT(data[0] == 0x01U, "MaskCode bit0 (charge) must be set while RUNNING");
    ASSERT(data[1] == 0x01U, "chg_sw must be 1 while RUNNING -- this is the bug: it used to always be 0");

    ChargeController_Stop(mock_tick);
    drive_ms(200U); /* STOPPING -> IDLE */
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_IDLE, "should have settled to IDLE after Stop");
    ASSERT(MockCan_GetLastTx(BMS_ID_CTRL_INFO, data), "Ctrl_INFO must have been sent");
    ASSERT(data[1] == 0U, "chg_sw must drop back to 0 once no longer RUNNING");

    printf("[PASS] test_bms_ctrl_info_allow_charge_wired\n");
    return true;
}

static bool test_relay_standalone_mode(void)
{
    printf("Running test_relay_standalone_mode...\n");
    ChargeCycleConfig_t cfg;
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_TONHE, &cfg), "setup failed");

    /* Standalone (no-BMS): reconfigure and never feed a single BMS frame --
     * proves the relay can close with no BMS ever present, per the explicit
     * requirement (previously BMS_IsOnline() was hard-required for ANY
     * mode, so this scenario's relay could never close at all). */
    cfg.charge_source_mode = CHARGE_SOURCE_STANDALONE_NO_BMS;
    ASSERT(ChargeCycleConfig_Set(&cfg), "failed to reconfigure standalone mode");
    g_sim_bms.transmitting = false;

    ASSERT(warmup_and_start(1500U, 4000U), "module never reached RUNNING");

    BMS_View_t bv;
    BMS_GetView(&bv);
    ASSERT(!bv.online, "BMS should never come online in this scenario");

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    float target = cv.target_voltage_v;
    ASSERT(target > 0.0f, "target voltage should be set");

    g_sim_module.voltage = target * 0.80f;
    drive_ms(200U);
    ChargeController_GetView(&cv);
    ASSERT(cv.relay_should_close == 0, "relay must stay open below 90% target voltage");

    g_sim_module.voltage = target * 0.97f;
    drive_ms(200U);
    ChargeController_GetView(&cv);
    ASSERT(cv.relay_should_close == 1, "relay should close on voltage alone -- no BMS required in standalone mode");

    /* Latch holds through a voltage sag here too, with no BMS involved at
     * all -- only leaving RUNNING could open it in this mode. */
    g_sim_module.voltage = target * 0.70f;
    drive_ms(200U);
    ChargeController_GetView(&cv);
    ASSERT(cv.relay_should_close == 1, "relay must stay closed when voltage sags after latching, standalone mode too");

    printf("[PASS] test_relay_standalone_mode\n");
    return true;
}

/* ChargeController_AcknowledgeCompletion(): after a normal standalone finish
 * (Vmax reached -> IDLE, stop_reason = VOLTAGE_REACHED), the HMI RESET press
 * must clear only the completion marker -- state stays IDLE, and it is a
 * no-op while still RUNNING. */
static bool test_acknowledge_completion(void)
{
    printf("Running test_acknowledge_completion...\n");
    ChargeCycleConfig_t cfg;
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_TONHE, &cfg), "setup failed");
    cfg.charge_source_mode = CHARGE_SOURCE_STANDALONE_NO_BMS;
    ASSERT(ChargeCycleConfig_Set(&cfg), "failed to reconfigure standalone mode");
    g_sim_bms.transmitting = false;

    ASSERT(warmup_and_start(1500U, 4000U), "module never reached RUNNING");

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_RUNNING, "should be RUNNING");
    float target = cv.target_voltage_v;

    /* No-op while RUNNING. */
    ChargeController_AcknowledgeCompletion();
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_RUNNING, "ack must be a no-op while RUNNING");

    /* Drive module voltage above Vmax and hold past the confirm window +
     * one more Process() so STOPPING -> IDLE. */
    g_sim_module.voltage = target * 1.05f;
    drive_ms(1000U + 500U + 200U);

    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_IDLE, "should be IDLE after Vmax reached");
    ASSERT(cv.stop_reason == CHARGE_STOP_VOLTAGE_REACHED,
           "stop_reason should be VOLTAGE_REACHED (the COMPLETE marker)");

    /* Operator presses RESET on the "complete" screen. */
    ChargeController_AcknowledgeCompletion();
    ChargeController_GetView(&cv);
    ASSERT(cv.stop_reason == CHARGE_STOP_NONE, "ack must clear the completion marker");
    ASSERT(cv.state == CHARGE_CTRL_STATE_IDLE, "ack must not change state");

    /* Idempotent / no-op when there is nothing to acknowledge. */
    ChargeController_AcknowledgeCompletion();
    ChargeController_GetView(&cv);
    ASSERT(cv.stop_reason == CHARGE_STOP_NONE, "second ack is a harmless no-op");

    printf("[PASS] test_acknowledge_completion\n");
    return true;
}

/* Setpoint ramp-up: the commanded current rises gradually toward the target
 * (CHARGE_CTRL_CURRENT_RAMP_A_PER_S), it is not a step. Observed via
 * GetView().applied_current_per_module_a (the sim reports a scripted module
 * current regardless of the limit, so applied_* is the signal). */
static bool test_current_ramp_up(void)
{
    printf("Running test_current_ramp_up...\n");
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_TONHE, NULL), "setup failed");
    set_healthy_bms(400.0f, 50);
    ASSERT(warmup_and_start(1500U, 4000U), "module never reached RUNNING");

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    float target_i = cv.target_current_per_module_a;
    ASSERT(target_i > 40.0f, "scenario needs a meaningful target (~100A)");
    ASSERT(cv.applied_current_per_module_a < 3.0f,
           "current ramp starts from ~0, not a jump to target");

    drive_ms(2000U);
    ChargeController_GetView(&cv);
    float i1 = cv.applied_current_per_module_a;
    ASSERT(i1 > 6.0f && i1 < 14.0f, "after 2s: ~5 A/s ramp rate");
    ASSERT(i1 < target_i - 5.0f, "still below target mid-ramp");

    drive_ms(25000U);
    ChargeController_GetView(&cv);
    ASSERT(fabsf(cv.applied_current_per_module_a - target_i) < 1.0f,
           "ramp reaches the full target (~20s for 100A at 5 A/s)");

    printf("[PASS] test_current_ramp_up\n");
    return true;
}

/* Two-rate voltage ramp: PRE-close the commanded voltage rises from 0 toward
 * the pack voltage at the fast CHARGE_CTRL_VOLTAGE_PRECLOSE_RAMP_V_PER_S
 * (which also gates when the relay arms); POST-close the Stage-1 -> vmax
 * rise uses the slow CHARGE_CTRL_VOLTAGE_RAMP_V_PER_S. Neither is a step. */
static bool test_voltage_ramp_up(void)
{
    printf("Running test_voltage_ramp_up...\n");
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_MAXWELL, NULL), "setup failed");
    set_healthy_bms(400.0f, 50);          /* pack 400V, cfg vmax 500V */
    ASSERT(warmup_and_start(1500U, 4000U), "module never reached RUNNING");

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.applied_voltage_v < 60.0f,
           "pre-close: voltage ramps from 0, not snapped to the pack voltage");
    ASSERT(!cv.relay_should_close, "relay not armed yet -- voltage still climbing");

    /* Pre-close ramp: 0 -> ~400V at 10 V/s (~40s) then the relay latches. */
    drive_ms(45000U);
    ChargeController_GetView(&cv);
    ASSERT(cv.relay_should_close, "relay latches once voltage reaches ~90% pack");
    ASSERT(cv.target_voltage_v > 490.0f, "post-latch target is vmax_v (~500V)");
    float v0 = cv.applied_voltage_v;
    ASSERT(v0 > 380.0f && v0 < 470.0f,
           "commanded voltage is near the pack V, has NOT jumped to vmax");

    /* Post-close ramp: slow rate toward vmax. */
    drive_ms(5000U);
    ChargeController_GetView(&cv);
    float v1 = cv.applied_voltage_v;
    ASSERT(v1 > v0 + 3.0f && v1 < cv.target_voltage_v,
           "post-close: slow ramp toward vmax");

    drive_ms(60000U);   /* 100V at 2 V/s = 50s + margin */
    ChargeController_GetView(&cv);
    ASSERT(fabsf(cv.applied_voltage_v - cv.target_voltage_v) < 1.0f,
           "voltage ramp reaches vmax");

    printf("[PASS] test_voltage_ramp_up\n");
    return true;
}

/* A DECREASE in the target (derating / lower manual setpoint) is applied
 * immediately -- the ramp only rate-limits the rise. */
static bool test_ramp_down_immediate(void)
{
    printf("Running test_ramp_down_immediate...\n");
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_TONHE, NULL), "setup failed");
    set_healthy_bms(400.0f, 50);

    drive_ms(1500U);
    ChargeController_SetManualTarget(400.0f, 100.0f);
    ASSERT(ChargeController_Start(CHARGE_CTRL_OWNER_PC, true, mock_tick),
           "manual start refused");
    drive_ms(600U);   /* reach RUNNING */

    drive_ms(9000U);  /* current ramps ~0 -> ~45A at 5 A/s */
    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.applied_current_per_module_a > 30.0f &&
           cv.applied_current_per_module_a < 70.0f, "mid-ramp");

    /* Operator lowers the manual current. */
    ChargeController_SetManualTarget(400.0f, 25.0f);
    drive_step(20U);  /* one control cycle */
    ChargeController_GetView(&cv);
    ASSERT(fabsf(cv.applied_current_per_module_a - 25.0f) < 3.0f,
           "decrease takes effect within one cycle, not rate-limited");

    printf("[PASS] test_ramp_down_immediate\n");
    return true;
}

/* Regression test for B-10: CHG_LIB_Process() used to service one module
 * per call via a round-robin index, so N modules took roughly N times as
 * long to reach RUNNING as a single module would (e.g. a 50ms retry
 * cadence stretched to 400ms with 8 modules). Confirms all N modules
 * reach RUNNING within the SAME timing budget a single module needs
 * (warmup_and_start() elsewhere in this file uses a 4000ms start
 * timeout for exactly one module). */
static bool test_multi_module_timing_budget(void)
{
    printf("Running test_multi_module_timing_budget...\n");
    /* 8 = MXR_MAX_MODULES / ChargeCycleConfig's max source_module_count --
     * the worst case the old round-robin (1 module serviced per
     * CHG_LIB_Process() call) had to handle.
     *
     * A flat "all N modules RUNNING within X ms" deadline turned out not
     * to discriminate reliably in this sim: Maxwell's own confirm-read
     * cadence (MXR_START_CONFIRM_WAIT_MS=100ms) is coarser than this
     * harness's 20ms drive step, so round-robin servicing one module
     * every 8*20ms=160ms still clears each individual wait-threshold check
     * on its first visit and the *total* time to all-RUNNING barely moves.
     * What round-robin actually does is STAGGER completion across modules
     * by one drive-step per round-robin slot (confirmed by temporarily
     * reverting to round-robin: modules finished 20ms apart each, in
     * round-robin order, for a 140ms = (8-1)*20ms spread start-to-finish)
     * -- so assert the real invariant instead: every module is serviced
     * every call, so they must all reach RUNNING within the same drive
     * step (or the next one, for scheduling/rounding slack), not spread
     * out over multiple round-robin cycles. */
    const uint8_t module_count = 8;
    const uint32_t max_completion_spread_ms = 40U; /* 2 drive steps */
    const uint32_t overall_timeout_ms = 2000U;

    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_MAXWELL, NULL), "setup failed");
    ChargeCycleConfig_t cfg;
    build_default_cfg(&cfg, CHARGE_MODULE_TYPE_MAXWELL);
    cfg.source_module_count = module_count;
    ASSERT(ChargeCycleConfig_Set(&cfg), "config rejected for multi-module setup");
    /* setup_scenario()'s sim_module_reset(&g_sim_module, ...) already ran
     * (as part of the driver-agnostic single-module reset) -- now arm the
     * remaining addr 2..module_count slots the ChargeCycleConfig_Set()
     * call above just registered (addr = i+1, group 0, per
     * charge_cycle_config.c's registration loop). */
    sim_module_reset_n(module_count, 1, 0);
    set_healthy_bms(400.0f, 50);

    drive_ms(1500U);
    ASSERT(ChargeController_Start(CHARGE_CTRL_OWNER_PC, false, mock_tick), "start refused");

    uint32_t reached_at_ms[8];
    bool seen[8] = {0};
    uint8_t seen_count = 0;

    for (uint32_t elapsed = 0; elapsed < overall_timeout_ms && seen_count < module_count; elapsed += 20U) {
        drive_step(20U);
        for (uint8_t i = 0; i < module_count; i++) {
            if (seen[i]) continue;
            CHG_LIB_ModuleView_t mv;
            if (CHG_LIB_GetModuleView(i, &mv) && mv.state == CHG_LIB_STATE_RUNNING) {
                seen[i] = true;
                reached_at_ms[i] = elapsed;
                seen_count++;
            }
        }
    }
    ASSERT(seen_count == module_count, "not all modules reached RUNNING within the overall timeout");

    uint32_t min_ms = reached_at_ms[0], max_ms = reached_at_ms[0];
    for (uint8_t i = 1; i < module_count; i++) {
        if (reached_at_ms[i] < min_ms) min_ms = reached_at_ms[i];
        if (reached_at_ms[i] > max_ms) max_ms = reached_at_ms[i];
    }
    ASSERT((max_ms - min_ms) <= max_completion_spread_ms,
           "modules finished RUNNING spread too far apart -- looks like round-robin starvation, not all-modules-per-call");

    printf("[PASS] test_multi_module_timing_budget\n");
    return true;
}

/* ================================================================== */
/* TonHe fault matrix (priority driver -- richest fault surface)       */
/* ================================================================== */

static bool test_tonhe_fault_matrix(void)
{
    printf("Running test_tonhe_fault_matrix...\n");
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_TONHE, NULL), "setup failed");
    set_healthy_bms(400.0f, 50);
    ASSERT(warmup_and_start(1500U, 4000U), "module never reached RUNNING");

    struct { uint16_t fault_bits; uint8_t pfc_bits; const char *label; } cases[] = {
        { (1U << 5),  0,            "over-temperature (status byte6 bit5)" },
        { (1U << 15), 0,            "short circuit (status byte6 bit15)" },
        { 0,          (1U << 7),    "PFC bus overvoltage (PFC byte bit7)" },
        { 0,          (1U << 1),    "mains frequency fault (PFC byte bit1)" },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        /* Fresh module per case: clear fault, let it recover to IDLE/STARTING
         * before injecting the next one, matching how process_module()'s
         * FAULT state only clears once alarm_flags reads back NONE. */
        g_sim_module.tonhe_fault_bits = 0;
        g_sim_module.tonhe_pfc_bits = 0;
        drive_ms(1500U);

        g_sim_module.tonhe_fault_bits = cases[i].fault_bits;
        g_sim_module.tonhe_pfc_bits = cases[i].pfc_bits;
        drive_ms(600U);

        CHG_LIB_ModuleView_t mv;
        ASSERT(CHG_LIB_GetModuleView(0, &mv), "module view unavailable");
        char msg[128];
        snprintf(msg, sizeof(msg), "expected FAULT for: %s", cases[i].label);
        if (mv.state != CHG_LIB_STATE_FAULT) {
            printf("[FAIL] test_tonhe_fault_matrix - %s (state=%d)\n", msg, (int)mv.state);
            return false;
        }
        if (mv.alarm_flags == CHG_LIB_ALARM_NONE) {
            printf("[FAIL] test_tonhe_fault_matrix - alarm_flags not decoded for: %s\n", cases[i].label);
            return false;
        }
    }

    printf("[PASS] test_tonhe_fault_matrix\n");
    return true;
}

/* ================================================================== */
/* BMS-behavior scenarios (driver-independent -- exercised once, TonHe) */
/* ================================================================== */

static bool test_bms_offline(void)
{
    printf("Running test_bms_offline...\n");
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_TONHE, NULL), "setup failed");
    set_healthy_bms(400.0f, 50);
    ASSERT(warmup_and_start(1500U, 4000U), "module never reached RUNNING");

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_RUNNING, "should be RUNNING before BMS loss");

    g_sim_bms.transmitting = false; /* simulate BMS comms loss */
    drive_ms(BMS_OFFLINE_TIMEOUT_MS + 500U);

    BMS_View_t bv;
    BMS_GetView(&bv);
    ASSERT(bv.state == BMS_STATE_OFFLINE, "BMS should be OFFLINE after timeout");
    ASSERT(!bv.online, "BMS view should report offline");

    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_FAULT, "controller should FAULT on BMS offline");
    ASSERT(cv.stop_reason == CHARGE_STOP_BMS_OFFLINE, "stop reason should be BMS_OFFLINE");
    ASSERT(!ChargeController_ResetFaultIfSafe(mock_tick),
           "Home/normal-charge reset must reject an offline BMS fault");

    g_sim_bms.transmitting = true;
    drive_ms(200U);
    ASSERT(ChargeController_ResetFaultIfSafe(mock_tick),
           "BMS offline fault should reset after the link recovers");
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_IDLE &&
           cv.fault_flags == CHARGE_CTRL_FAULT_NONE,
           "recovered BMS reset must return to IDLE without fault");

    printf("[PASS] test_bms_offline\n");
    return true;
}

/* Regression test: BMS_ALARM_BMS_OFFLINE used to latch forever once set --
 * nothing cleared it on the OFFLINE->ONLINE recovery path, so a BMS view
 * could show online=true while still carrying a stale "offline" alarm bit
 * (found via real-hardware HIL testing, not host-sim -- the host sims never
 * exercised an offline-then-recover sequence before this test). */
static bool test_bms_offline_then_recovers(void)
{
    printf("Running test_bms_offline_then_recovers...\n");
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_TONHE, NULL), "setup failed");
    set_healthy_bms(400.0f, 50);
    ASSERT(warmup_and_start(1500U, 4000U), "module never reached RUNNING");

    g_sim_bms.transmitting = false; /* simulate BMS comms loss */
    drive_ms(BMS_OFFLINE_TIMEOUT_MS + 500U);

    BMS_View_t bv;
    BMS_GetView(&bv);
    ASSERT(bv.state == BMS_STATE_OFFLINE, "BMS should be OFFLINE after timeout");
    ASSERT(bv.alarm_flags & BMS_ALARM_BMS_OFFLINE, "BMS_ALARM_BMS_OFFLINE should be set while offline");

    /* Resume transmitting -- BMS should recover to ONLINE. */
    g_sim_bms.transmitting = true;
    drive_ms(200U);

    BMS_GetView(&bv);
    ASSERT(bv.online, "BMS should be back online after data resumes");
    ASSERT(bv.state == BMS_STATE_ONLINE, "BMS state should return to ONLINE");
    ASSERT(!(bv.alarm_flags & BMS_ALARM_BMS_OFFLINE),
           "BMS_ALARM_BMS_OFFLINE must clear on recovery -- must not latch forever");

    printf("[PASS] test_bms_offline_then_recovers\n");
    return true;
}

static bool test_bms_critical_alarm(void)
{
    printf("Running test_bms_critical_alarm...\n");
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_TONHE, NULL), "setup failed");
    set_healthy_bms(400.0f, 50);
    ASSERT(warmup_and_start(1500U, 4000U), "module never reached RUNNING");

    /* over_chg_curr severity=3 (severe) -- only >=2 counts as active per
     * bms_core.c's map_alarm_field(). */
    g_sim_bms.over_chg_curr = 3;
    drive_ms(1000U);

    BMS_View_t bv;
    BMS_GetView(&bv);
    ASSERT(bv.alarm_flags & BMS_ALARM_OVER_CHG_CURR, "BMS_ALARM_OVER_CHG_CURR should be set");

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_FAULT, "controller should FAULT on critical BMS alarm");
    ASSERT(cv.stop_reason == CHARGE_STOP_BMS_ALARM, "stop reason should be BMS_ALARM");

    printf("[PASS] test_bms_critical_alarm\n");
    return true;
}

static bool test_bms_stale_but_online(void)
{
    printf("Running test_bms_stale_but_online...\n");
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_TONHE, NULL), "setup failed");
    set_healthy_bms(400.0f, 50);
    ASSERT(warmup_and_start(1500U, 4000U), "module never reached RUNNING");

    /* Stop refreshing but stay under BMS_OFFLINE_TIMEOUT_MS(5000) so the
     * connection itself doesn't time out -- only staleness (>2000ms). */
    g_sim_bms.transmitting = false;
    drive_ms(BMS_STALE_THRESHOLD_MS + 300U);

    BMS_View_t bv;
    BMS_GetView(&bv);
    ASSERT(bv.online, "BMS should still be online (under the 5s offline timeout)");
    ASSERT(BMS_IsDataStale(), "BMS data-status stale should be set");
    ASSERT((bv.alarm_flags & BMS_ALARM_BMS_OFFLINE) == 0U,
           "stale data must not be reported as BMS offline");

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_RUNNING, "stale data is a soft warning, must not fault a running cycle");

    printf("[PASS] test_bms_stale_but_online\n");
    return true;
}

/* Regression test for the CHG_LIB_SetModuleConfig wiring gap found during
 * review: ChargeCycleConfig_Set() must seed each Maxwell module's rated
 * current from cfg.module_i_max_a immediately on CHG_LIB_AddModule(), so
 * the very first CHG_LIB_REG_SET_CURR_LIMIT ratio the driver sends is
 * computed against the real rating -- not the hardcoded
 * MXR_DEFAULT_RATED_CURRENT_A(20A) fallback -- even before the module has
 * answered a single CAN poll (register 0x0012 self-reports it, but only
 * once the module has responded at least once; this covers that window).
 *
 * Config here: module_i_max_a=200A, BMS-controlled, healthy pack sits in
 * cell-voltage band 1_2 (cell_curr_1_c=1.0 -> full imax_c=1.0C * 100Ah =
 * 100A target). Per the Maxwell protocol PDF sec 2.3.1 ("current limit =
 * required / rated"): correct ratio = 100/200 = 0.5. The old bug would
 * have computed 100/20 = 5.0, clamped to 1.0 by current_limit_to_ratio()
 * -- i.e. the module would have been told to run at its full rated
 * current instead of half. */
static bool test_rated_current_seeded_from_config(void)
{
    printf("Running test_rated_current_seeded_from_config...\n");
    ChargeCycleConfig_t cfg;
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_MAXWELL, &cfg), "setup failed");
    ASSERT(cfg.module_i_max_a == 200.0f, "test assumes the default 200A config");
    set_healthy_bms(400.0f, 50);

    ASSERT(warmup_and_start(1500U, 4000U), "module never reached RUNNING");

    /* The current setpoint now ramps 0 -> target at CHARGE_CTRL_CURRENT_RAMP_
     * A_PER_S (5 A/s); drive past a full 100A ramp (+margin) so the final
     * commanded ratio is the config value, not a mid-ramp fraction. */
    drive_ms(25000U);

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    float expected_ratio = cv.target_current_per_module_a / cfg.module_i_max_a;

    ASSERT(g_sim_module.last_set_curr_limit_ratio > 0.0f, "SET_CURR_LIMIT was never sent");
    ASSERT(fabsf(g_sim_module.last_set_curr_limit_ratio - expected_ratio) < 0.01f,
           "current-limit ratio must be computed against the configured rated current, not the 20A fallback");
    ASSERT(g_sim_module.last_set_curr_limit_ratio < 0.99f,
           "ratio should not be clamped to 1.0 -- that's the old-bug fallback symptom (100/20 clamped)");

    printf("[PASS] test_rated_current_seeded_from_config\n");
    return true;
}

/* Regression test: Maxwell's poll cycle now includes 0x0005 (input DC
 * voltage) and 0x004B (input working mode), added 2026-08-29 after
 * cross-checking the driver against the vendor PDF's full register table
 * found they were defined (priv/chg_lib_protocol.h) but never polled.
 * IDLE's keepalive poll only advances one register per 1000ms, so this
 * drives long enough (MXR_POLL_REG_COUNT=17 seconds' worth, plus margin)
 * to guarantee a full cycle completes before checking. */
static bool test_maxwell_input_diagnostics_polled(void)
{
    printf("Running test_maxwell_input_diagnostics_polled...\n");
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_MAXWELL, NULL), "setup failed");
    set_healthy_bms(400.0f, 50);

    /* Stay in IDLE (should_run stays false -- no ChargeController_Start())
     * long enough for the round-robin poll to cycle through all 17
     * registers at least once. */
    drive_ms(20000U);

    CHG_LIB_ModuleView_t mv;
    ASSERT(CHG_LIB_GetModuleView(0, &mv), "module view unavailable");
    ASSERT(mv.input_dc_voltage > 0.0f, "input_dc_voltage (0x0005) should have been polled by now");
    ASSERT(mv.input_mode != 0U, "input_mode (0x004B) should have been polled by now");
    ASSERT(mv.input_mode == 3U, "sim reports 3 = three-phase AC");

    printf("[PASS] test_maxwell_input_diagnostics_polled\n");
    return true;
}

/* Regression test: a module the operator has explicitly told to stop used
 * to just sit forever in whatever comms-health state (WARNING/OFFLINE/
 * RECOVERING) it happened to be in at the moment of STOP, if the module
 * never sent another frame -- xxx_stop() only handled RUNNING/STARTING,
 * and CHG_LIB_FSM_CheckOfflineTimeout()'s own early-return for OFFLINE/
 * RECOVERING meant nothing else would ever move it out of there either.
 * Confirmed via real HIL testing 2026-08-29 with the module simulator
 * fully silenced: State stayed RECOVERING indefinitely after both STOP and
 * EMERGENCY_STOP, even though the operator's intent was unambiguous.
 * Fixed by (a) gating the offline-timeout watchdog on should_run (a
 * stopped module shouldn't be graded on comms health it's not being asked
 * to have), and (b) xxx_stop() force-transitioning WARNING/OFFLINE/
 * RECOVERING straight to IDLE. FAULT is deliberately untouched by either
 * change -- still gated by its own 5-clean-read debounce (B-08); this
 * fix is scoped to comms-health states, not real hardware faults. */
static bool test_stop_forces_idle_from_recovering(void)
{
    printf("Running test_stop_forces_idle_from_recovering...\n");
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_TONHE, NULL), "setup failed");
    set_healthy_bms(400.0f, 50);
    ASSERT(warmup_and_start(1500U, 4000U), "module never reached RUNNING");

    /* Module goes permanently silent (comms lost) -- WARNING(2s) ->
     * OFFLINE(10s) -> RECOVERING(+3s more) per the TonHe driver's own
     * timeouts, and stays there since it never sends another frame. */
    g_sim_module.silent = true;
    drive_ms(16000U);

    CHG_LIB_ModuleView_t mv;
    ASSERT(CHG_LIB_GetModuleView(0, &mv), "module view unavailable");
    ASSERT(mv.state == CHG_LIB_STATE_RECOVERING, "module should be RECOVERING (still silent, mid-session)");

    /* Operator presses STOP. */
    ASSERT(CHG_LIB_Stop(0), "CHG_LIB_Stop should succeed");
    ASSERT(CHG_LIB_GetModuleView(0, &mv), "module view unavailable");
    ASSERT(mv.state == CHG_LIB_STATE_IDLE,
           "module must go straight to IDLE on STOP, not stay stuck in RECOVERING");
    ASSERT(!mv.running, "module must not read as running after STOP");

    /* Keep the module silent and keep driving -- the now should_run-gated
     * watchdog must NOT drag it back into WARNING/OFFLINE/RECOVERING just
     * because comms are still down; it's not wanted right now. */
    drive_ms(16000U);
    ASSERT(CHG_LIB_GetModuleView(0, &mv), "module view unavailable");
    ASSERT(mv.state == CHG_LIB_STATE_IDLE, "module must stay IDLE while stopped, even with comms still down");
    ASSERT(!mv.online, "online should honestly read false -- comms really are down, just not driving the state machine");

    printf("[PASS] test_stop_forces_idle_from_recovering\n");
    return true;
}

static bool test_jack_temp_derating_and_trip(void)
{
    printf("Running test_jack_temp_derating_and_trip...\n");
    ChargeCycleConfig_t cfg;
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_TONHE, &cfg), "setup failed");
    cfg.protect_jack_temp_enabled = 1;
    cfg.protect_jack_temp_delay_s = 2;
    cfg.protect_jack_temp_threshold_c = 60.0f;
    cfg.protect_jack_temp_trip_c = 75.0f;
    cfg.protect_jack_temp_power_limit_pct = 50.0f;
    ASSERT(ChargeCycleConfig_Set(&cfg), "set config failed");

    set_healthy_bms(400.0f, 50);
    g_sim_bms.max_cell_mv = 3100; /* Stage band 1_2 allows full current */
    ASSERT(warmup_and_start(1500U, 4000U), "module never reached RUNNING");

    /* Initially jack temp is 25C (normal) */
    ChargeController_SetJackTempC(25.0f);
    drive_ms(500U);
    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_RUNNING, "controller should be RUNNING");
    ASSERT(cv.derating == 0, "should not be derating at 25C");

    /* Step 1: Temp reaches threshold (60C). After 2s, derating triggers (current reduced to 50%) */
    ChargeController_SetJackTempC(62.0f);
    drive_ms(2500U);
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_RUNNING, "controller should still be RUNNING in derating");
    ASSERT(cv.derating == 1, "derating should be active above threshold");
    ASSERT(fabsf(cv.target_current_total_a - 50.0f) < 2.0f, "current should be derated to 50%");

    /* Step 2: Temp continues rising and hits trip threshold (75C). After 2s, hard protection FAULT triggers */
    ChargeController_SetJackTempC(76.0f);
    drive_ms(2500U);
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_FAULT, "controller must transition to FAULT at trip temperature");
    ASSERT(cv.fault_flags & CHARGE_CTRL_FAULT_PROTECT_JACK_TEMP, "CHARGE_CTRL_FAULT_PROTECT_JACK_TEMP must be set");
    ASSERT(cv.target_current_total_a == 0.0f, "target current must be 0 in FAULT");

    printf("[PASS] test_jack_temp_derating_and_trip\n");
    return true;
}

static bool test_config_admin_pin_validation(void)
{
    printf("Running test_config_admin_pin_validation...\n");
    ChargeCycleConfig_t cfg;
    ChargeCycleConfig_GetDefaults(&cfg);
    ASSERT(cfg.version == CHARGE_CYCLE_CONFIG_VERSION, "defaults use current config version");
    ASSERT(cfg.admin_pin == DEFAULT_ADMIN_PIN, "defaults install the documented admin PIN");

    cfg.admin_pin = 99999U;
    ASSERT(!ChargeCycleConfig_Set(&cfg), "five-digit PIN must be rejected");
    cfg.admin_pin = 100000U;
    ASSERT(ChargeCycleConfig_Set(&cfg), "lowest valid six-digit PIN accepted");
    cfg.admin_pin = 1000000U;
    ASSERT(!ChargeCycleConfig_Set(&cfg), "seven-digit PIN must be rejected");

    printf("[PASS] test_config_admin_pin_validation\n");
    return true;
}

static bool test_stage_threshold_validation(void)
{
    printf("Running test_stage_threshold_validation...\n");
    ChargeCycleConfig_t cfg;

    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_MAXWELL, &cfg), "setup failed");

    /* Equal cell thresholds would make the lower band unreachable. */
    cfg.cell_volt_2_v = cfg.cell_volt_1_v;
    ASSERT(!ChargeCycleConfig_Set(&cfg), "equal cell thresholds must be rejected");

    /* Equal SOC thresholds have the same unreachable-band problem. */
    build_default_cfg(&cfg, CHARGE_MODULE_TYPE_MAXWELL);
    cfg.cell_volt_enabled = 0;
    cfg.soc_enabled = 1;
    cfg.soc_1_pct = 20.0f;
    cfg.soc_2_pct = 40.0f;
    cfg.soc_3_pct = 60.0f;
    cfg.soc_4_pct = 80.0f;
    cfg.soc_5_pct = 80.0f;
    ASSERT(!ChargeCycleConfig_Set(&cfg), "equal SOC thresholds must be rejected");

    /* Temperature delta may equal the smallest gap, but not exceed it. */
    build_default_cfg(&cfg, CHARGE_MODULE_TYPE_MAXWELL);
    cfg.temp_enabled = 1;
    cfg.temp_1_c = 10.0f;
    cfg.temp_2_c = 20.0f;
    cfg.temp_3_c = 30.0f;
    cfg.temp_4_c = 40.0f;
    cfg.temp_5_c = 50.0f;
    cfg.temp_delta_c = 10.0f;
    ASSERT(ChargeCycleConfig_Set(&cfg), "temp delta equal to minimum gap must be accepted");

    cfg.temp_delta_c = 10.1f;
    ASSERT(!ChargeCycleConfig_Set(&cfg), "temp delta larger than minimum gap must be rejected");

    printf("[PASS] test_stage_threshold_validation\n");
    return true;
}

static bool test_precharge_bms_recovery_hold(void)
{
    printf("Running test_precharge_bms_recovery_hold...\n");
    ChargeCycleConfig_t cfg;
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_MAXWELL, &cfg), "setup failed");

    cfg.vlow_v = 35.0f;
    cfg.ilow_c = 0.5f; /* 100 Ah -> 50 A total / one module */
    cfg.module_u_min_v = 30.0f;
    cfg.module_u_max_v = 60.0f;
    cfg.module_i_min_a = 1.0f;
    cfg.module_i_max_a = 100.0f;
    ASSERT(ChargeCycleConfig_Set(&cfg), "pre-charge config rejected");

    /* Module communication is healthy while the exhausted BMS is silent. */
    g_sim_bms.transmitting = false;
    drive_ms(1500U);
    ASSERT(ChargeController_StartPrecharge(CHARGE_CTRL_OWNER_DWIN, mock_tick),
           "pre-charge must start while BMS is offline");
    drive_ms(4500U); /* pre-close voltage ramp reaches 35 V at 10 V/s */

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_PRECHARGE, "controller must stay in PRECHARGE");
    ASSERT(fabsf(cv.target_voltage_v - cfg.vlow_v) < 0.01f, "uses Vlow target");
    ASSERT(fabsf(cv.target_current_total_a - 50.0f) < 0.01f, "Ilow C-rate converts to total current");
    ASSERT(cv.relay_should_close != 0U, "relay arms from module Vlow without BMS online");

    /* BATT_ST1 alone is not recovery: CELL_VOLT must also be fresh. */
    uint8_t batt_st1[8] = { 0x5E, 0x01, 0xA0, 0x0F, 50U, 0U, 0U, 0U };
    BMS_FeedFrame(0U, 0x02F4U, batt_st1, sizeof(batt_st1));
    BMS_Process(mock_tick);
    ASSERT(!BMS_HasFreshPrechargeData(mock_tick), "BATT_ST1 alone must not recover pre-charge");

    set_healthy_bms(35.0f, 5U);
    g_sim_bms.low_cell_volt = 2U; /* expected low-voltage alarm remains non-critical */
    g_sim_bms.transmitting = true;
    drive_ms(30000U);

    /* Losing BMS during the hold resets it; 30 seconds after recovery is not
     * enough to finish a new full 60-second hold. */
    g_sim_bms.transmitting = false;
    drive_ms(1000U);
    g_sim_bms.transmitting = true;
    drive_ms(30000U);
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_PRECHARGE, "BMS loss must reset, not complete, hold");

    drive_ms(PRECHARGE_HOLD_MS);
    drive_ms(40U); /* STOPPING -> IDLE on the controlled-stop path */
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_IDLE, "60 second recovered hold must controlled-stop to IDLE");
    ASSERT(cv.stop_reason == CHARGE_STOP_PRECHARGE_COMPLETE, "pre-charge completion reason retained");

    printf("[PASS] test_precharge_bms_recovery_hold\n");
    return true;
}

static bool test_precharge_start_invalid_state(void)
{
    printf("Running test_precharge_start_invalid_state...\n");
    ChargeCycleConfig_t cfg;
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_MAXWELL, &cfg), "setup failed");
    set_healthy_bms(400.0f, 50);
    ASSERT(warmup_and_start(1500U, 4000U), "module never reached RUNNING");

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_RUNNING, "must be in RUNNING");

    /* Cannot start precharge while running */
    ASSERT(!ChargeController_StartPrecharge(CHARGE_CTRL_OWNER_DWIN, mock_tick),
           "StartPrecharge must fail when not IDLE");
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_RUNNING, "state must remain RUNNING");

    /* Stop charge -> STOPPING */
    ChargeController_Stop(mock_tick);
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_STOPPING, "must be in STOPPING");
    ASSERT(!ChargeController_StartPrecharge(CHARGE_CTRL_OWNER_DWIN, mock_tick),
           "StartPrecharge must fail while STOPPING");

    /* Complete stop to IDLE */
    drive_ms(2000U);
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_IDLE, "must reach IDLE");

    printf("[PASS] test_precharge_start_invalid_state\n");
    return true;
}

static bool test_precharge_config_boundaries(void)
{
    printf("Running test_precharge_config_boundaries...\n");
    ChargeCycleConfig_t cfg;

    /* Case A: Vlow < module_u_min_v */
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_MAXWELL, &cfg), "setup failed");
    cfg.vlow_v = 20.0f;
    cfg.module_u_min_v = 30.0f;
    cfg.module_u_max_v = 60.0f;
    ASSERT(ChargeCycleConfig_Set(&cfg), "config set failed");
    ASSERT(!ChargeController_StartPrecharge(CHARGE_CTRL_OWNER_DWIN, mock_tick),
           "Vlow < Vmin must be rejected");
    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_FAULT, "must enter FAULT");
    ASSERT((cv.fault_flags & CHARGE_CTRL_FAULT_INVALID_CONFIG) != 0,
           "fault flag must be INVALID_CONFIG");

    /* Reset to IDLE */
    ChargeController_Stop(mock_tick);
    drive_ms(100U);

    /* Case B: Vlow > module_u_max_v */
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_MAXWELL, &cfg), "setup failed");
    cfg.vlow_v = 75.0f;
    cfg.module_u_min_v = 30.0f;
    cfg.module_u_max_v = 60.0f;
    ASSERT(ChargeCycleConfig_Set(&cfg), "config set failed");
    ASSERT(!ChargeController_StartPrecharge(CHARGE_CTRL_OWNER_DWIN, mock_tick),
           "Vlow > Vmax must be rejected");
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_FAULT, "must enter FAULT");
    ASSERT((cv.fault_flags & CHARGE_CTRL_FAULT_INVALID_CONFIG) != 0,
           "fault flag must be INVALID_CONFIG");

    /* Reset to IDLE */
    ChargeController_Stop(mock_tick);
    drive_ms(100U);

    /* Case C: current per module < min_a */
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_MAXWELL, &cfg), "setup failed");
    cfg.vlow_v = 45.0f;
    cfg.ilow_c = 0.001f; /* 100 Ah * 0.001 = 0.1 A < 1.0 A */
    cfg.module_u_min_v = 30.0f;
    cfg.module_u_max_v = 60.0f;
    cfg.module_i_min_a = 1.0f;
    cfg.module_i_max_a = 50.0f;
    ASSERT(ChargeCycleConfig_Set(&cfg), "config set failed");
    ASSERT(!ChargeController_StartPrecharge(CHARGE_CTRL_OWNER_DWIN, mock_tick),
           "Ilow < Imin must be rejected");
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_FAULT, "must enter FAULT");

    ChargeController_Stop(mock_tick);
    drive_ms(100U);

    printf("[PASS] test_precharge_config_boundaries\n");
    return true;
}

static bool test_precharge_zero_or_faulty_module(void)
{
    printf("Running test_precharge_zero_or_faulty_module...\n");
    ChargeCycleConfig_t cfg;
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_MAXWELL, &cfg), "setup failed");
    cfg.vlow_v = 45.0f;
    cfg.ilow_c = 0.2f;
    cfg.module_u_min_v = 30.0f;
    cfg.module_u_max_v = 60.0f;
    cfg.module_i_min_a = 1.0f;
    cfg.module_i_max_a = 50.0f;
    ASSERT(ChargeCycleConfig_Set(&cfg), "config set failed");

    /* No modules online */
    g_sim_module.silent = true;
    drive_ms(16000U);
    ASSERT(!ChargeController_StartPrecharge(CHARGE_CTRL_OWNER_DWIN, mock_tick),
           "Precharge must fail with 0 modules online");
    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_FAULT, "must enter FAULT");

    /* Module in fault */
    ChargeController_Stop(mock_tick);
    drive_ms(100U);
    g_sim_module.silent = false;
    drive_ms(1000U);
    g_sim_module.maxwell_alarm_raw = (1U << 28);
    drive_ms(16000U); /* Maxwell polls 1 register/sec in IDLE; reg 5 is ALARM_STATUS */
    ASSERT(!ChargeController_StartPrecharge(CHARGE_CTRL_OWNER_DWIN, mock_tick),
           "Precharge must fail with module in fault");

    ChargeController_Stop(mock_tick);
    drive_ms(100U);
    printf("[PASS] test_precharge_zero_or_faulty_module\n");
    return true;
}

static bool test_precharge_fault_reset_requires_safe_conditions(void)
{
    printf("Running test_precharge_fault_reset_requires_safe_conditions...\n");
    ChargeCycleConfig_t cfg;
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_MAXWELL, &cfg), "setup failed");
    cfg.vlow_v = 45.0f;
    cfg.ilow_c = 0.2f;
    cfg.module_u_min_v = 30.0f;
    cfg.module_u_max_v = 60.0f;
    cfg.module_i_min_a = 1.0f;
    cfg.module_i_max_a = 50.0f;
    ASSERT(ChargeCycleConfig_Set(&cfg), "config set failed");

    g_sim_module.silent = true;
    drive_ms(16000U);
    ASSERT(!ChargeController_StartPrecharge(CHARGE_CTRL_OWNER_DWIN, mock_tick),
           "pre-charge must fail without an active module");

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_FAULT, "start failure must enter FAULT");
    ASSERT(!ChargeController_ResetFaultIfSafe(mock_tick),
           "reset must be rejected while the module condition persists");

    /* RESET is intentionally idempotent while the root cause remains. A
     * repeated press must not clear the controller or start any output. */
    ChargeCtrlState_t state_after_failed_reset = cv.state;
    uint32_t faults_after_failed_reset = cv.fault_flags;
    ASSERT(!ChargeController_ResetFaultIfSafe(mock_tick),
           "repeated reset must remain rejected while the fault persists");
    ChargeController_GetView(&cv);
    ASSERT(cv.state == state_after_failed_reset &&
           cv.fault_flags == faults_after_failed_reset,
           "repeated reset must not change state or fault flags");

    /* Let the module recover while the controller remains faulted. Reset is
     * then allowed without using ChargeController_Stop() as an unconditional
     * fault bypass. */
    g_sim_module.silent = false;
    drive_ms(16000U);
    ASSERT(ChargeController_ResetFaultIfSafe(mock_tick),
           "reset must succeed after module recovery");
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_IDLE, "safe reset must return to IDLE");
    ASSERT(cv.fault_flags == CHARGE_CTRL_FAULT_NONE, "safe reset must clear controller fault");
    ASSERT(!ChargeController_ResetFaultIfSafe(mock_tick),
           "reset after recovery must be harmless while already IDLE");

    /* Emergency stop is intentionally not an ordinary pre-charge retry. */
    ChargeController_EmergencyStop(mock_tick);
    ASSERT(!ChargeController_ResetFaultIfSafe(mock_tick),
           "emergency stop must not be cleared by pre-charge reset");

    printf("[PASS] test_precharge_fault_reset_requires_safe_conditions\n");
    return true;
}

static bool test_precharge_voltage_ramp_and_contactor_interlock(void)
{
    printf("Running test_precharge_voltage_ramp_and_contactor_interlock...\n");
    ChargeCycleConfig_t cfg;
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_MAXWELL, &cfg), "setup failed");
    cfg.vlow_v = 40.0f;
    cfg.ilow_c = 0.2f; /* 100 Ah * 0.2 = 20 A */
    cfg.module_u_min_v = 30.0f;
    cfg.module_u_max_v = 60.0f;
    cfg.module_i_min_a = 1.0f;
    cfg.module_i_max_a = 50.0f;
    ASSERT(ChargeCycleConfig_Set(&cfg), "config set failed");

    g_sim_bms.transmitting = false; /* BMS offline */
    drive_ms(1500U);

    ASSERT(ChargeController_StartPrecharge(CHARGE_CTRL_OWNER_DWIN, mock_tick),
           "Precharge start must succeed");

    /* Slew rate is 10 V/s. At 1s, voltage is ~10V, which is < 39V (Vlow - 1V).
     * Contactor must NOT close! */
    drive_ms(1000U);
    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_PRECHARGE, "must be in PRECHARGE");
    ASSERT(!cv.relay_should_close, "relay must not close before reaching Vlow - 1V");

    /* After 4.5s, voltage reaches 40V (>= 39V). Contactor MUST arm and latch! */
    drive_ms(3500U);
    ChargeController_GetView(&cv);
    ASSERT(cv.relay_should_close != 0U, "relay should close when within 1V of Vlow");

    /* Manual stop from user */
    ChargeController_StopPrecharge(mock_tick);
    drive_ms(100U);
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_IDLE || cv.state == CHARGE_CTRL_STATE_STOPPING,
           "StopPrecharge must initiate controlled stop");

    printf("[PASS] test_precharge_voltage_ramp_and_contactor_interlock\n");
    return true;
}

static bool test_precharge_incomplete_bms_frames(void)
{
    printf("Running test_precharge_incomplete_bms_frames...\n");
    ChargeCycleConfig_t cfg;
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_MAXWELL, &cfg), "setup failed");
    cfg.vlow_v = 35.0f;
    cfg.ilow_c = 0.2f;
    cfg.module_u_min_v = 30.0f;
    cfg.module_u_max_v = 60.0f;
    cfg.module_i_min_a = 1.0f;
    cfg.module_i_max_a = 50.0f;
    ASSERT(ChargeCycleConfig_Set(&cfg), "config set failed");

    g_sim_bms.transmitting = false;
    drive_ms(1500U);
    ASSERT(ChargeController_StartPrecharge(CHARGE_CTRL_OWNER_DWIN, mock_tick), "start failed");
    drive_ms(4500U); /* reach 35V */

    /* Case A: Send only BATT_ST1 (0x02F4) */
    uint8_t batt_st1[8] = { 0x5E, 0x01, 0xA0, 0x0F, 50U, 0U, 0U, 0U };
    BMS_FeedFrame(0U, 0x02F4U, batt_st1, sizeof(batt_st1));
    BMS_Process(mock_tick);
    ASSERT(!BMS_HasFreshPrechargeData(mock_tick), "BATT_ST1 alone must not be fresh precharge data");

    drive_ms(1000U);
    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_PRECHARGE, "must stay in PRECHARGE");

    /* Case B: Send only CELL_VOLT (0x02F6) while BATT_ST1 goes stale */
    drive_ms(3000U); /* let BATT_ST1 expire */
    uint8_t cell_volt[8] = { 0x20, 0x0D, 0x10, 0x0D, 0x00, 0x00, 0x00, 0x00 };
    BMS_FeedFrame(0U, 0x02F6U, cell_volt, sizeof(cell_volt));
    BMS_Process(mock_tick);
    ASSERT(!BMS_HasFreshPrechargeData(mock_tick), "CELL_VOLT alone must not be fresh precharge data");

    ChargeController_StopPrecharge(mock_tick);
    drive_ms(100U);
    printf("[PASS] test_precharge_incomplete_bms_frames\n");
    return true;
}

static bool test_precharge_bms_critical_fault_injection(void)
{
    printf("Running test_precharge_bms_critical_fault_injection...\n");
    ChargeCycleConfig_t cfg;
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_MAXWELL, &cfg), "setup failed");
    cfg.vlow_v = 35.0f;
    cfg.ilow_c = 0.2f;
    cfg.module_u_min_v = 30.0f;
    cfg.module_u_max_v = 60.0f;
    cfg.module_i_min_a = 1.0f;
    cfg.module_i_max_a = 50.0f;
    ASSERT(ChargeCycleConfig_Set(&cfg), "config set failed");

    g_sim_bms.transmitting = false;
    drive_ms(1500U);
    ASSERT(ChargeController_StartPrecharge(CHARGE_CTRL_OWNER_DWIN, mock_tick), "start failed");
    drive_ms(4500U); /* reach 35V */

    /* BMS wakes up with a CRITICAL over-temperature alarm */
    set_healthy_bms(35.0f, 5);
    g_sim_bms.temp_cell_high_chg = 2U; /* Critical alarm level >= 2 */
    g_sim_bms.transmitting = true;
    drive_ms(100U); /* Process BMS frame */

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_FAULT, "Critical BMS alarm must trip FAULT immediately");
    ASSERT((cv.fault_flags & CHARGE_CTRL_FAULT_BMS_ALARM) != 0, "fault must be BMS_ALARM");

    /* A critical BMS fault cannot be reset after the BMS goes stale: without
     * fresh BATT_ST1 + CELL_VOLT, the controller cannot prove the cause gone. */
    g_sim_bms.transmitting = false;
    drive_ms(BMS_OFFLINE_TIMEOUT_MS + 500U);
    ASSERT(!ChargeController_ResetFaultIfSafe(mock_tick),
           "stale/offline BMS must reject reset of a critical BMS fault");

    ChargeController_Stop(mock_tick);
    drive_ms(100U);
    printf("[PASS] test_precharge_bms_critical_fault_injection\n");
    return true;
}

static bool test_precharge_module_can_loss(void)
{
    printf("Running test_precharge_module_can_loss...\n");
    ChargeCycleConfig_t cfg;
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_MAXWELL, &cfg), "setup failed");
    cfg.vlow_v = 35.0f;
    cfg.ilow_c = 0.2f;
    cfg.module_u_min_v = 30.0f;
    cfg.module_u_max_v = 60.0f;
    cfg.module_i_min_a = 1.0f;
    cfg.module_i_max_a = 50.0f;
    ASSERT(ChargeCycleConfig_Set(&cfg), "config set failed");

    g_sim_bms.transmitting = false;
    drive_ms(1500U);
    ASSERT(ChargeController_StartPrecharge(CHARGE_CTRL_OWNER_DWIN, mock_tick), "start failed");
    drive_ms(4500U);

    /* Kill module CAN communication: requires 10s driver offline timeout + 10s controller mismatch debounce */
    g_sim_module.silent = true;
    drive_ms(22000U); /* Exceed module timeout (10s) + mismatch timer (10s) */

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_FAULT, "Module CAN loss must trip FAULT");

    ChargeController_Stop(mock_tick);
    drive_ms(100U);
    printf("[PASS] test_precharge_module_can_loss\n");
    return true;
}

static bool test_temp_stage_asymmetric_hysteresis(void)
{
    printf("Running test_temp_stage_asymmetric_hysteresis...\n");
    ChargeCycleConfig_t cfg;
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_MAXWELL, &cfg), "setup failed");

    cfg.temp_enabled = 1;
    cfg.temp_1_c = 10.0f;
    cfg.temp_2_c = 20.0f;
    cfg.temp_3_c = 40.0f;
    cfg.temp_4_c = 50.0f;
    cfg.temp_5_c = 60.0f;
    cfg.temp_curr_1_c = 0.2f;
    cfg.temp_curr_2_c = 0.5f;
    cfg.temp_curr_3_c = 1.0f;
    cfg.temp_curr_4_c = 0.3f;
    cfg.temp_delta_c = 3.0f;
    cfg.cell_volt_enabled = 0;
    cfg.soc_enabled = 0;
    ASSERT(ChargeCycleConfig_Set(&cfg), "config set failed");

    set_healthy_bms(400.0f, 50);
    g_sim_bms.max_cell_temp_c = 45.0f; /* In band 3_4: [40, 50) */
    ASSERT(warmup_and_start(1500U, 4000U), "module never reached RUNNING");

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.active_stage_band == CHARGE_STAGE_BAND_3_4, "initial temp 45C should be in band 3_4");
    ASSERT(fabsf(cv.target_current_total_a - 100.0f) < 1.0f, "band 3_4 should allow 1.0C (100A)");

    /* 1. Rise to exactly 50.0C (threshold 4_5): with asymmetric hysteresis, must transition immediately */
    g_sim_bms.max_cell_temp_c = 50.0f;
    drive_ms(600U);
    ChargeController_GetView(&cv);
    ASSERT(cv.active_stage_band == CHARGE_STAGE_BAND_4_5, "rising temp to 50C must trip immediately to band 4_5");
    ASSERT(fabsf(cv.target_current_total_a - 30.0f) < 1.0f, "band 4_5 should limit to 0.3C (30A)");

    /* 2. Cool down to 49.0C: 49.0 >= 50.0 - 3.0 (47.0C) -> must retain band 4_5 */
    g_sim_bms.max_cell_temp_c = 49.0f;
    drive_ms(600U);
    ChargeController_GetView(&cv);
    ASSERT(cv.active_stage_band == CHARGE_STAGE_BAND_4_5, "cooling to 49C must remain in band 4_5 (delta not met)");

    /* 3. Cool down to 48.0C: 48.0 >= 47.0C -> still in band 4_5 */
    g_sim_bms.max_cell_temp_c = 48.0f;
    drive_ms(600U);
    ChargeController_GetView(&cv);
    ASSERT(cv.active_stage_band == CHARGE_STAGE_BAND_4_5, "cooling to 48C must remain in band 4_5 (delta not met)");

    /* 4. Cool down to 46.0C: 46.0 < 47.0C -> recovers to band 3_4 */
    g_sim_bms.max_cell_temp_c = 46.0f;
    drive_ms(600U);
    ChargeController_GetView(&cv);
    ASSERT(cv.active_stage_band == CHARGE_STAGE_BAND_3_4, "cooling to 46C (< lower_thresh - delta) must recover to band 3_4");
    ASSERT(fabsf(cv.target_current_total_a - 100.0f) < 1.0f, "recovered band 3_4 should restore 100A");

    /* 5. Jump to 60.0C (threshold 5: ABOVE_MAX): must inhibit immediately */
    g_sim_bms.max_cell_temp_c = 60.0f;
    drive_ms(600U);
    ChargeController_GetView(&cv);
    ASSERT(cv.active_stage_band == CHARGE_STAGE_BAND_ABOVE_MAX, "rising to 60C must trip immediately to ABOVE_MAX");
    ASSERT(cv.inhibit == 1, "ABOVE_MAX must set inhibit");

    /* 6. Cool down to 58.0C: 58.0 >= 60.0 - 3.0 (57.0C) -> still ABOVE_MAX */
    g_sim_bms.max_cell_temp_c = 58.0f;
    drive_ms(600U);
    ChargeController_GetView(&cv);
    ASSERT(cv.active_stage_band == CHARGE_STAGE_BAND_ABOVE_MAX, "cooling to 58C must stay in ABOVE_MAX (delta not met)");

    /* 7. Cool down to 56.0C: 56.0 < 57.0C -> recovers to band 4_5 */
    g_sim_bms.max_cell_temp_c = 56.0f;
    drive_ms(600U);
    ChargeController_GetView(&cv);
    ASSERT(cv.active_stage_band == CHARGE_STAGE_BAND_4_5, "cooling to 56C must recover from ABOVE_MAX to band 4_5");
    ASSERT(cv.inhibit == 0, "inhibit must clear on recovery");
    ASSERT(fabsf(cv.target_current_total_a - 30.0f) < 1.0f, "recovered band 4_5 current target restored");

    printf("[PASS] test_temp_stage_asymmetric_hysteresis\n");
    return true;
}

static bool test_cell_volt_stage_delta_t_debounce(void)
{
    printf("Running test_cell_volt_stage_delta_t_debounce...\n");
    ChargeCycleConfig_t cfg;
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_MAXWELL, &cfg), "setup failed");

    /* Cell voltage stages with 3.0s delta_t debounce */
    cfg.cell_volt_enabled = 1;
    cfg.cell_volt_delta_t_s = 3.0f;
    cfg.cell_volt_1_v = 3.00f;
    cfg.cell_volt_2_v = 3.30f;
    cfg.cell_volt_3_v = 3.50f;
    cfg.cell_volt_4_v = 3.65f;
    cfg.cell_volt_5_v = 3.80f;
    cfg.cell_curr_1_c = 1.00f; /* 100A */
    cfg.cell_curr_2_c = 0.70f; /* 70A */
    cfg.cell_curr_3_c = 0.40f; /* 40A */
    cfg.cell_curr_4_c = 0.20f; /* 20A */
    cfg.temp_enabled = 0;
    cfg.soc_enabled = 0;
    ASSERT(ChargeCycleConfig_Set(&cfg), "config set failed");

    set_healthy_bms(400.0f, 50);
    g_sim_bms.max_cell_mv = 3100; /* In band 1_2: [3.00V, 3.30V) -> 100A */
    ASSERT(warmup_and_start(1500U, 4000U), "module never reached RUNNING");

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.active_stage_band == CHARGE_STAGE_BAND_1_2, "initial cell 3.10V should seed band 1_2");
    ASSERT(fabsf(cv.target_current_total_a - 100.0f) < 1.0f, "band 1_2 should allow 1.0C (100A)");

    /* 1. Spike to 3.40V (Band 2_3: >= 3.30V) for only 1.5s (less than 3.0s delta_t) */
    g_sim_bms.max_cell_mv = 3400;
    drive_ms(1500U);
    ChargeController_GetView(&cv);
    ASSERT(cv.active_stage_band == CHARGE_STAGE_BAND_1_2, "voltage spike for 1.5s (< 3.0s) must NOT transition to band 2_3");
    ASSERT(fabsf(cv.target_current_total_a - 100.0f) < 1.0f, "current must remain 100A while waiting for debounce");

    /* 2. Spike drops back to 3.20V (Band 1_2) -> cancels candidate timer */
    g_sim_bms.max_cell_mv = 3200;
    drive_ms(1000U);
    ChargeController_GetView(&cv);
    ASSERT(cv.active_stage_band == CHARGE_STAGE_BAND_1_2, "candidate timer canceled, still band 1_2");

    /* 3. Voltage rises to 3.40V and STAYS for full 3.0s (3200ms) */
    g_sim_bms.max_cell_mv = 3400;
    drive_ms(2000U);
    ChargeController_GetView(&cv);
    ASSERT(cv.active_stage_band == CHARGE_STAGE_BAND_1_2, "after 2.0s, still waiting for 3.0s debounce");
    drive_ms(1200U); /* total 3.2s >= 3.0s */
    ChargeController_GetView(&cv);
    ASSERT(cv.active_stage_band == CHARGE_STAGE_BAND_2_3, "after 3.2s >= delta_t, must confirm transition to band 2_3");
    ASSERT(fabsf(cv.target_current_total_a - 70.0f) < 1.0f, "band 2_3 target current is 70A");

    /* 4. Voltage rises to 3.85V (ABOVE_MAX: >= 3.80V) for only 1.5s -> must NOT stop yet */
    g_sim_bms.max_cell_mv = 3850;
    drive_ms(1500U);
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_RUNNING, "ABOVE_MAX spike for 1.5s must not stop charge cycle early");
    ASSERT(cv.active_stage_band == CHARGE_STAGE_BAND_2_3, "still in band 2_3 during debounce");

    /* 5. Stay at 3.85V for remaining time (> 3.0s total) -> confirms ABOVE_MAX and stops cycle */
    drive_ms(1800U); /* 1.5s + 1.8s = 3.3s >= 3.0s */
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_STOPPING || cv.state == CHARGE_CTRL_STATE_IDLE,
           "after delta_t at ABOVE_MAX, cycle must stop (STOPPING or settled to IDLE)");
    ASSERT(cv.stop_reason == CHARGE_STOP_CELL_VOLTAGE_REACHED, "stop reason must be CELL_VOLTAGE_REACHED");

    printf("[PASS] test_cell_volt_stage_delta_t_debounce\n");
    return true;
}

static bool test_soc_stage_delta_t_debounce(void)
{
    printf("Running test_soc_stage_delta_t_debounce...\n");
    ChargeCycleConfig_t cfg;
    ASSERT(setup_scenario(CHARGE_MODULE_TYPE_MAXWELL, &cfg), "setup failed");

    /* SOC stages with 2.0s delta_t debounce */
    cfg.soc_enabled = 1;
    cfg.soc_delta_t_s = 2.0f;
    cfg.soc_1_pct = 20.0f;
    cfg.soc_2_pct = 50.0f;
    cfg.soc_3_pct = 80.0f;
    cfg.soc_4_pct = 90.0f;
    cfg.soc_5_pct = 99.0f;
    cfg.soc_curr_1_c = 1.00f; /* 100A */
    cfg.soc_curr_2_c = 0.70f; /* 70A */
    cfg.soc_curr_3_c = 0.40f; /* 40A */
    cfg.soc_curr_4_c = 0.20f; /* 20A */
    cfg.cell_volt_enabled = 0;
    cfg.temp_enabled = 0;
    ASSERT(ChargeCycleConfig_Set(&cfg), "config set failed");

    set_healthy_bms(400.0f, 30); /* In band 1_2: [20%, 50%) -> 100A */
    ASSERT(warmup_and_start(1500U, 4000U), "module never reached RUNNING");

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.active_stage_band == CHARGE_STAGE_BAND_1_2, "initial SOC 30% should seed band 1_2");

    /* Spike to 60% (Band 2_3: >= 50%) for 1.0s (< 2.0s debounce) */
    g_sim_bms.soc_pct = 60;
    drive_ms(1000U);
    ChargeController_GetView(&cv);
    ASSERT(cv.active_stage_band == CHARGE_STAGE_BAND_1_2, "SOC spike for 1.0s must not transition early");

    /* Stay at 60% for another 1.2s (total 2.2s >= 2.0s debounce) */
    drive_ms(1200U);
    ChargeController_GetView(&cv);
    ASSERT(cv.active_stage_band == CHARGE_STAGE_BAND_2_3, "after debounce, confirmed transition to band 2_3");
    ASSERT(fabsf(cv.target_current_total_a - 70.0f) < 1.0f, "band 2_3 target current 70A");

    /* Spike to 100% (ABOVE_MAX: >= 99%) for 1.0s (< 2.0s debounce) -> must NOT stop */
    g_sim_bms.soc_pct = 100;
    drive_ms(1000U);
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_RUNNING, "SOC 100% for 1.0s must not stop cycle early");

    /* Stay at 100% for another 1.2s (total 2.2s >= 2.0s debounce) -> stop cycle */
    drive_ms(1200U);
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_STOPPING || cv.state == CHARGE_CTRL_STATE_IDLE,
           "after delta_t at 100% SOC, cycle must stop (STOPPING or settled to IDLE)");
    ASSERT(cv.stop_reason == CHARGE_STOP_SOC_REACHED, "stop reason must be SOC_REACHED");

    printf("[PASS] test_soc_stage_delta_t_debounce\n");
    return true;
}

/* ================================================================== */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== Charger E2E Simulation Test (BMS + module CAN -> MCU -> app view) ===\n");
    bool pass = true;

    pass &= test_driver_happy_path(CHARGE_MODULE_TYPE_TONHE, "tonhe");
    pass &= test_driver_stage_derating(CHARGE_MODULE_TYPE_TONHE, "tonhe");
    pass &= test_temp_stage_asymmetric_hysteresis();
    pass &= test_cell_volt_stage_delta_t_debounce();
    pass &= test_soc_stage_delta_t_debounce();
    pass &= test_driver_module_fault(CHARGE_MODULE_TYPE_TONHE, "tonhe");
    pass &= test_driver_fault_recovery_debounce(CHARGE_MODULE_TYPE_TONHE, "tonhe");
    pass &= test_tonhe_fault_matrix();
    pass &= test_distinct_module_alarm_matrix();

    pass &= test_driver_happy_path(CHARGE_MODULE_TYPE_LIANMING, "lianming");
    pass &= test_driver_module_fault(CHARGE_MODULE_TYPE_LIANMING, "lianming");
    pass &= test_driver_fault_recovery_debounce(CHARGE_MODULE_TYPE_LIANMING, "lianming");

    pass &= test_driver_happy_path(CHARGE_MODULE_TYPE_MAXWELL, "maxwell");
    pass &= test_driver_module_fault(CHARGE_MODULE_TYPE_MAXWELL, "maxwell");
    pass &= test_driver_fault_recovery_debounce(CHARGE_MODULE_TYPE_MAXWELL, "maxwell");
    pass &= test_rated_current_seeded_from_config();
    pass &= test_maxwell_input_diagnostics_polled();
    pass &= test_stop_forces_idle_from_recovering();

    pass &= test_bms_offline();
    pass &= test_bms_offline_then_recovers();
    pass &= test_bms_critical_alarm();
    pass &= test_bms_high_pack_volt_alarm_stops_module();
    pass &= test_bms_stale_but_online();

    pass &= test_relay_bms_mode();
    pass &= test_relay_arms_off_bms_voltage_not_target();
    pass &= test_relay_stays_closed_until_current_settles();
    pass &= test_relay_opens_on_timeout_if_current_never_settles();
    pass &= test_relay_opens_immediately_on_emergency_stop();
    pass &= test_bms_ctrl_info_allow_charge_wired();
    pass &= test_relay_standalone_mode();
    pass &= test_acknowledge_completion();
    pass &= test_current_ramp_up();
    pass &= test_voltage_ramp_up();
    pass &= test_ramp_down_immediate();
    pass &= test_jack_temp_derating_and_trip();
    pass &= test_config_admin_pin_validation();
    pass &= test_stage_threshold_validation();
    pass &= test_precharge_bms_recovery_hold();
    pass &= test_precharge_start_invalid_state();
    pass &= test_precharge_config_boundaries();
    pass &= test_precharge_zero_or_faulty_module();
    pass &= test_precharge_fault_reset_requires_safe_conditions();
    pass &= test_precharge_voltage_ramp_and_contactor_interlock();
    pass &= test_precharge_incomplete_bms_frames();
    pass &= test_precharge_bms_critical_fault_injection();
    pass &= test_precharge_module_can_loss();

    pass &= test_multi_module_timing_budget();

    if (pass) {
        printf("ALL TESTS PASSED.\n");
        return 0;
    }
    printf("TESTS FAILED.\n");
    return 1;
}
