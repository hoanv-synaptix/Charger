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
    cfg->cell_volt_delta_v = 0.02f;
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
    ASSERT(fabsf(cv.target_voltage_v - cfg.vmax_v) < 0.01f, "target voltage should equal vmax_v (no BMS override)");

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
    float target = cv.target_voltage_v;
    ASSERT(target > 0.0f, "target voltage should be set");

    /* Below 90% target: relay must stay open even though the controller is
     * RUNNING and the BMS is otherwise healthy. */
    g_sim_module.voltage = target * 0.80f;
    drive_ms(200U);
    ChargeController_GetView(&cv);
    ASSERT(cv.relay_should_close == 0, "relay must stay open below 90% target voltage");

    /* >=90% target, BMS healthy (bms_relay_allow default true from
     * sim_bms_reset): relay closes (arms the latch). */
    g_sim_module.voltage = target * 0.95f;
    drive_ms(200U);
    ChargeController_GetView(&cv);
    ASSERT(cv.relay_should_close == 1, "relay should close once voltage >=90% and BMS reports safe");

    /* Voltage sags back below 90% (e.g. CV-phase current taper) -- this is
     * explicitly NOT a fault once latched, relay must stay closed. */
    g_sim_module.voltage = target * 0.70f;
    drive_ms(200U);
    ChargeController_GetView(&cv);
    ASSERT(cv.relay_should_close == 1, "relay must stay closed when voltage sags after latching -- not a re-open trigger");
    ASSERT(cv.state == CHARGE_CTRL_STATE_RUNNING, "still RUNNING, this is normal charging behaviour");

    /* BMS's own relay-allow flag (BmsSwSta) goes false while voltage is low
     * (post-sag) and the controller is otherwise still RUNNING -- this IS a
     * fault and must open the relay even though it's latched closed. */
    g_sim_bms.bms_relay_allow = false;
    drive_ms(700U); /* BmsSwSta resend interval is 500ms */
    ChargeController_GetView(&cv);
    ASSERT(cv.relay_should_close == 0, "relay must open when BMS reports its relay-allow flag false, even while latched");
    ASSERT(cv.state == CHARGE_CTRL_STATE_RUNNING, "a false BMS relay-allow flag alone must not fault the whole cycle");

    printf("[PASS] test_relay_bms_mode\n");
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

    g_sim_module.voltage = target * 0.95f;
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
    ASSERT(bv.alarm_flags & BMS_ALARM_STALE_DATA, "BMS_ALARM_STALE_DATA should be set");

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

/* ================================================================== */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("=== Charger E2E Simulation Test (BMS + module CAN -> MCU -> app view) ===\n");
    bool pass = true;

    pass &= test_driver_happy_path(CHARGE_MODULE_TYPE_TONHE, "tonhe");
    pass &= test_driver_stage_derating(CHARGE_MODULE_TYPE_TONHE, "tonhe");
    pass &= test_driver_module_fault(CHARGE_MODULE_TYPE_TONHE, "tonhe");
    pass &= test_driver_fault_recovery_debounce(CHARGE_MODULE_TYPE_TONHE, "tonhe");
    pass &= test_tonhe_fault_matrix();

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
    pass &= test_bms_stale_but_online();

    pass &= test_relay_bms_mode();
    pass &= test_relay_standalone_mode();

    pass &= test_multi_module_timing_budget();

    if (pass) {
        printf("ALL TESTS PASSED.\n");
        return 0;
    }
    printf("TESTS FAILED.\n");
    return 1;
}
