/**
 * @file  test_alarm_e2e.c
 * @brief Host end-to-end tests for App/Alarm/alarm.c against the real
 *        charge controller + simulated BMS / TonHe module CAN.
 *
 * Driving loop mirrors App_Loop()'s 20 ms control block order, with
 * Alarm_Process() slotted in right after ChargeController_Process() (exactly
 * where App/System/app_main.c calls it). Follows the ASSERT / bool-test-fn /
 * main() AND-chain convention of test/test_logic.c.
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
#include "alarm.h"
#include "dwin_alarm_text.h"

#include "sim_can_modules.h"
#include "sim_bms.h"

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

static void drive_step(uint32_t step_ms)
{
    mock_tick += step_ms;
    sim_bms_tick(mock_tick);
    sim_module_tick(SIM_DRV_TONHE, mock_tick);
    CHG_LIB_Process(mock_tick);
    BMS_Process(mock_tick);
    ChargeController_Process(mock_tick);
    Alarm_Process(mock_tick);
}

static void drive_ms(uint32_t total_ms)
{
    for (uint32_t e = 0; e < total_ms; e += 20U) drive_step(20U);
}

static bool alarm_active(AlarmCode_t c)
{
    AlarmView_t v;
    Alarm_GetView(&v);
    return (v.active_mask & (1ULL << c)) != 0ULL;
}

/* Was `code` ever raised in the event log this scenario? */
static bool alarm_logged_raise(AlarmCode_t code)
{
    AlarmLogEntry_t log[ALARM_LOG_DEPTH];
    uint8_t n = Alarm_GetLog(log, ALARM_LOG_DEPTH);
    for (uint8_t i = 0; i < n; i++) {
        if (log[i].code == (uint16_t)code && log[i].event == 1U) return true;
    }
    return false;
}

static void healthy_bms(float pack_v)
{
    g_sim_bms.pack_voltage_v = pack_v;
    g_sim_bms.pack_current_a = 0.0f;
    g_sim_bms.soc_pct = 50;
    g_sim_bms.max_cell_mv = 3200; g_sim_bms.max_cv_no = 1;
    g_sim_bms.min_cell_mv = 3180; g_sim_bms.min_cv_no = 2;
    g_sim_bms.max_cell_temp_c = 25.0f; g_sim_bms.min_cell_temp_c = 24.0f;
    g_sim_bms.avg_cell_temp_c = 24.5f;
    g_sim_bms.cap_remain_x0_1ah = 500; g_sim_bms.rate_cap_x0_1ah = 1000;
    g_sim_bms.soh_pct = 100;
    g_sim_bms.chg_volt_request_v = 500.0f; g_sim_bms.chg_curr_request_a = 50.0f;
    g_sim_bms.bms_relay_allow = true;
    g_sim_bms.transmitting = true;
}

static bool setup(ChargeCycleConfig_t *cfg_out)
{
    static bool registered = false;
    if (!registered) {
        CHG_LIB_RegisterDriver(CHG_LIB_DRV_MAXWELL, CHG_LIB_MaxwellDriverOps());
        CHG_LIB_RegisterDriver(CHG_LIB_DRV_LIANMING, CHG_LIB_LianmingDriverOps());
        CHG_LIB_RegisterDriver(CHG_LIB_DRV_TONHE, CHG_LIB_TonheDriverOps());
        registered = true;
    }
    mock_tick = 0;
    sim_install_backend(SIM_DRV_TONHE);
    sim_module_reset(&g_sim_module, 1, 0);
    g_sim_module.current_override = true;   /* tests drive current directly */
    g_sim_module.rated_current = 100.0f;
    sim_bms_reset(&g_sim_bms);

    BMS_Init();
    ChargeCycleConfig_Init();
    ChargeController_Init();
    Alarm_Init();

    ChargeCycleConfig_t cfg;
    ChargeCycleConfig_GetDefaults(&cfg);
    cfg.module_type = CHARGE_MODULE_TYPE_TONHE;
    cfg.source_module_count = 1;
    cfg.charge_source_mode = CHARGE_SOURCE_BMS_CONTROLLED;
    cfg.battery_capacity_ah = 100.0f;
    cfg.imin_c = 0.05f; cfg.imax_c = 1.0f;
    cfg.vmin_v = 300.0f; cfg.vmax_v = 500.0f;
    cfg.module_u_min_v = 30.0f; cfg.module_u_max_v = 550.0f;
    cfg.module_i_min_a = 1.0f;  cfg.module_i_max_a = 200.0f;
    cfg.cell_volt_enabled = 0; cfg.temp_enabled = 0; cfg.soc_enabled = 0;
    if (!ChargeCycleConfig_Set(&cfg)) { printf("[FAIL] config rejected\n"); return false; }
    if (cfg_out) *cfg_out = cfg;
    return true;
}

/* Warm BMS+module online, START, and drive to controller RUNNING. */
static bool start_running(void)
{
    drive_ms(1500U);
    if (!ChargeController_Start(CHARGE_CTRL_OWNER_PC, false, mock_tick)) {
        printf("[FAIL] start refused\n");
        return false;
    }
    for (uint32_t e = 0; e < 3000U; e += 20U) {
        drive_step(20U);
        ChargeCtrlView_t v; ChargeController_GetView(&v);
        if (v.state == CHARGE_CTRL_STATE_RUNNING) return true;
    }
    return false;
}

/* Bring the DC path to "carrying real current at target voltage". */
static void establish_load(float target_v, float per_mod_a)
{
    g_sim_module.voltage = target_v;
    g_sim_module.current = per_mod_a;
    g_sim_bms.pack_current_a = per_mod_a;
    drive_ms(400U);
}

/* ================================================================== */

static bool test_happy_path_no_alarm(void)
{
    printf("Running test_happy_path_no_alarm...\n");
    ASSERT(setup(NULL), "setup");
    healthy_bms(400.0f);
    ASSERT(start_running(), "controller never RUNNING");
    establish_load(400.0f, 40.0f);
    drive_ms(2000U);

    AlarmView_t v; Alarm_GetView(&v);
    ASSERT(v.active_count == 0, "no alarm expected on happy path");
    ASSERT(v.highest_action == ALARM_ACT_INFO, "no action expected");
    printf("[PASS] test_happy_path_no_alarm\n");
    return true;
}

static bool test_module_specific_alarms_and_dwin_text(void)
{
    printf("Running test_module_specific_alarms_and_dwin_text...\n");
    ASSERT(strcmp(DWIN_Alarm_GetCodeString(ALARM_MOD_FAN_FAULT), "E016") == 0,
           "fan fault must map to E016");
    ASSERT(strcmp(DWIN_Alarm_GetCodeString(ALARM_MOD_AC_OVER_VOLT), "E017") == 0,
           "AC input overvoltage must map to E017");

    uint8_t desc_len = 0U;
    ASSERT(DWIN_Alarm_GetDescUtf16(ALARM_MOD_FAN_FAULT, &desc_len) != NULL && desc_len > 0U,
           "fan fault description must be available");
    ASSERT(DWIN_Alarm_GetDescUtf16(ALARM_MOD_AC_OVER_VOLT, &desc_len) != NULL && desc_len > 0U,
           "AC input overvoltage description must be available");

    ASSERT(setup(NULL), "setup");
    healthy_bms(400.0f);
    ASSERT(start_running(), "controller never RUNNING");
    g_sim_module.tonhe_fault_bits = (1U << 6);
    drive_ms(800U);
    ASSERT(alarm_active(ALARM_MOD_FAN_FAULT), "fan fault unified alarm not active");

    ASSERT(setup(NULL), "setup");
    healthy_bms(400.0f);
    ASSERT(start_running(), "controller never RUNNING");
    g_sim_module.tonhe_fault_bits = (1U << 2);
    drive_ms(800U);
    ASSERT(alarm_active(ALARM_MOD_AC_OVER_VOLT), "AC input overvoltage unified alarm not active");

    AlarmView_t view;
    Alarm_GetView(&view);
    ASSERT(view.highest_action == ALARM_ACT_STOP, "new module alarms must request STOP");
    printf("[PASS] test_module_specific_alarms_and_dwin_text\n");
    return true;
}

static bool test_cv_taper_no_false_load_lost(void)
{
    printf("Running test_cv_taper_no_false_load_lost...\n");
    ASSERT(setup(NULL), "setup");
    healthy_bms(400.0f);
    ASSERT(start_running(), "controller never RUNNING");
    establish_load(400.0f, 40.0f);

    /* CV taper: module voltage stays at target, current decays smoothly, and
     * the BMS still reports pack current flowing. Must NOT trip DC_LOAD_LOST. */
    for (int i = 0; i < 20; i++) {
        float ia = 40.0f - (float)i * 1.8f;
        if (ia < 1.0f) ia = 1.0f;
        g_sim_module.current = ia;
        g_sim_bms.pack_current_a = ia;
        drive_ms(100U);
    }
    ASSERT(!alarm_active(ALARM_DC_LOAD_LOST), "CV taper wrongly tripped DC_LOAD_LOST");
    printf("[PASS] test_cv_taper_no_false_load_lost\n");
    return true;
}

static bool test_dc_load_lost_hot_unplug(void)
{
    printf("Running test_dc_load_lost_hot_unplug...\n");
    ASSERT(setup(NULL), "setup");
    healthy_bms(400.0f);
    ASSERT(start_running(), "controller never RUNNING");
    establish_load(400.0f, 40.0f);

    /* Connector pulled: current collapses to ~0, module voltage pegs at the
     * commanded target, BMS pack current also drops to 0. */
    ChargeCtrlView_t cv; ChargeController_GetView(&cv);
    g_sim_module.voltage = cv.target_voltage_v;
    g_sim_module.current = 0.0f;
    g_sim_bms.pack_current_a = 0.0f;
    drive_ms(1500U);

    ASSERT(alarm_logged_raise(ALARM_DC_LOAD_LOST), "DC_LOAD_LOST never raised");
    ASSERT(alarm_active(ALARM_DC_LOAD_LOST), "DC_LOAD_LOST should latch active");
    ChargeController_GetView(&cv);
    ASSERT(cv.state != CHARGE_CTRL_STATE_RUNNING, "controller should have been stopped");
    printf("[PASS] test_dc_load_lost_hot_unplug\n");
    return true;
}

static bool test_dc_out_not_established(void)
{
    printf("Running test_dc_out_not_established...\n");
    ASSERT(setup(NULL), "setup");
    healthy_bms(400.0f);
    ASSERT(start_running(), "controller never RUNNING");

    /* Relay commanded closed (module ramps to pack voltage) but the external
     * contactor never closes -> no current ever flows. */
    g_sim_module.voltage = 400.0f;
    g_sim_module.current = 0.0f;
    g_sim_bms.pack_current_a = 0.0f;
    drive_ms(7000U);

    ASSERT(alarm_logged_raise(ALARM_DC_OUT_NOT_ESTABLISHED), "DC_OUT_NOT_ESTABLISHED never raised");
    ChargeCtrlView_t cv; ChargeController_GetView(&cv);
    ASSERT(cv.state != CHARGE_CTRL_STATE_RUNNING, "controller should have been stopped");
    printf("[PASS] test_dc_out_not_established\n");
    return true;
}

static bool test_bms_comm_lost_mid_charge(void)
{
    printf("Running test_bms_comm_lost_mid_charge...\n");
    ASSERT(setup(NULL), "setup");
    healthy_bms(400.0f);
    ASSERT(start_running(), "controller never RUNNING");
    establish_load(400.0f, 40.0f);

    g_sim_bms.transmitting = false;   /* BMS link drops */
    drive_ms(8000U);

    ASSERT(alarm_logged_raise(ALARM_BMS_COMM_LOST), "BMS_COMM_LOST never raised");
    ChargeCtrlView_t cv; ChargeController_GetView(&cv);
    ASSERT(cv.state != CHARGE_CTRL_STATE_RUNNING, "controller should have stopped");
    printf("[PASS] test_bms_comm_lost_mid_charge\n");
    return true;
}

static bool test_bms_no_pack_voltage(void)
{
    printf("Running test_bms_no_pack_voltage...\n");
    ASSERT(setup(NULL), "setup");
    healthy_bms(10.0f);              /* BMS online, but pack ~ 0 V */
    ASSERT(start_running(), "controller never RUNNING");
    drive_ms(1500U);

    ASSERT(alarm_logged_raise(ALARM_BMS_NO_PACK_VOLTAGE), "BMS_NO_PACK_VOLTAGE never raised");
    ChargeCtrlView_t cv; ChargeController_GetView(&cv);
    ASSERT(cv.state != CHARGE_CTRL_STATE_RUNNING, "controller should have stopped");
    printf("[PASS] test_bms_no_pack_voltage\n");
    return true;
}

static bool test_bms_critical_alarm_mirrored(void)
{
    printf("Running test_bms_critical_alarm_mirrored...\n");
    ASSERT(setup(NULL), "setup");
    healthy_bms(400.0f);
    ASSERT(start_running(), "controller never RUNNING");

    g_sim_bms.high_cell_volt = 3;   /* severity 3 -> BMS critical alarm */
    drive_ms(1000U);

    ASSERT(alarm_active(ALARM_BMS_HIGH_CELL_VOLT) || alarm_logged_raise(ALARM_BMS_HIGH_CELL_VOLT),
           "BMS high-cell-volt not mirrored");
    ChargeCtrlView_t cv; ChargeController_GetView(&cv);
    ASSERT(cv.state != CHARGE_CTRL_STATE_RUNNING, "existing BMS-alarm stop path must still fire");
    printf("[PASS] test_bms_critical_alarm_mirrored\n");
    return true;
}

static bool test_module_ac_undervolt_mirrored_and_derived(void)
{
    printf("Running test_module_ac_undervolt_mirrored_and_derived...\n");
    ASSERT(setup(NULL), "setup");
    healthy_bms(400.0f);
    ASSERT(start_running(), "controller never RUNNING");

    g_sim_module.tonhe_fault_bits = (1U << 0);   /* input undervoltage */
    drive_ms(1500U);

    ASSERT(alarm_logged_raise(ALARM_MOD_AC_UNDER_VOLT), "module AC-undervolt mirror missing");
    ASSERT(alarm_logged_raise(ALARM_AC_UNDERVOLT), "derived AC_UNDERVOLT missing");
    printf("[PASS] test_module_ac_undervolt_mirrored_and_derived\n");
    return true;
}

static bool test_acknowledge_clears_latched(void)
{
    printf("Running test_acknowledge_clears_latched...\n");
    ASSERT(setup(NULL), "setup");
    healthy_bms(400.0f);
    ASSERT(start_running(), "controller never RUNNING");
    establish_load(400.0f, 40.0f);

    ChargeCtrlView_t cv; ChargeController_GetView(&cv);
    g_sim_module.voltage = cv.target_voltage_v;
    g_sim_module.current = 0.0f;
    g_sim_bms.pack_current_a = 0.0f;
    drive_ms(1500U);
    ASSERT(alarm_active(ALARM_DC_LOAD_LOST), "precondition: DC_LOAD_LOST latched");

    /* Latching: the alarm must NOT auto-clear just because the charge has
     * stopped and the raw condition is no longer observable -- only an
     * operator acknowledge clears it. */
    drive_ms(3000U);
    ASSERT(alarm_active(ALARM_DC_LOAD_LOST), "latched alarm must survive until acknowledged");

    Alarm_Acknowledge(mock_tick);
    drive_step(20U);
    ASSERT(!alarm_active(ALARM_DC_LOAD_LOST), "acknowledge should clear the latched alarm");
    printf("[PASS] test_acknowledge_clears_latched\n");
    return true;
}

static bool test_start_with_no_module_or_bms_reports_fault_code(void)
{
    printf("Running test_start_with_no_module_or_bms_reports_fault_code...\n");
    ASSERT(setup(NULL), "setup");
    /* Do NOT send BMS or module frames (offline / none connected) */
    bool started = ChargeController_Start(CHARGE_CTRL_OWNER_DWIN, false, mock_tick);
    ASSERT(!started, "start should be refused when no modules/bms");

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_FAULT, "controller must transition to FAULT");

    /* Step alarm processing with controller view */
    drive_step(20U);

    AlarmView_t av;
    Alarm_GetView(&av);
    ASSERT(av.active_count > 0, "active alarms must be > 0");
    ASSERT(av.worst_code == ALARM_CTRL_NO_MODULE || av.worst_code == ALARM_BMS_COMM_LOST,
           "worst_code must report NO_MODULE or BMS_COMM_LOST");
    ASSERT(av.worst_code != ALARM_NONE, "worst_code must NOT be ALARM_NONE");

    printf("[PASS] test_start_with_no_module_or_bms_reports_fault_code (worst_code=%u)\n",
           (unsigned)av.worst_code);
    return true;
}

int main(void)
{
    bool ok = true;
    ok &= test_happy_path_no_alarm();
    ok &= test_module_specific_alarms_and_dwin_text();
    ok &= test_cv_taper_no_false_load_lost();
    ok &= test_dc_load_lost_hot_unplug();
    ok &= test_dc_out_not_established();
    ok &= test_bms_comm_lost_mid_charge();
    ok &= test_bms_no_pack_voltage();
    ok &= test_bms_critical_alarm_mirrored();
    ok &= test_module_ac_undervolt_mirrored_and_derived();
    ok &= test_acknowledge_clears_latched();
    ok &= test_start_with_no_module_or_bms_reports_fault_code();

    if (ok) { printf("\nALL TESTS PASSED.\n"); return 0; }
    printf("\nSOME TESTS FAILED.\n");
    return 1;
}
