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

static bool setup(ChargeCycleConfig_t *cfg_out);
static bool start_running(void);

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

/* ALM_INFO field order and two-bit severity encoding are taken from the
 * vendor BMS PDF: 0x07F4 Standard, little-endian, fields packed from bit 0
 * in the order below. This helper deliberately writes only the selected
 * field so each test proves the raw-to-normalized mapping independently. */
static void set_bms_alarm_severity(uint8_t field, uint8_t severity)
{
    switch (field) {
        case 0:  g_sim_bms.low_pack_volt       = severity; break;
        case 1:  g_sim_bms.low_cell_volt       = severity; break;
        case 2:  g_sim_bms.high_pack_volt      = severity; break;
        case 3:  g_sim_bms.high_cell_volt      = severity; break;
        case 4:  g_sim_bms.temp_cell_high_chg  = severity; break;
        case 5:  g_sim_bms.temp_cell_high_dchg = severity; break;
        case 6:  g_sim_bms.temp_cell_low_chg   = severity; break;
        case 7:  g_sim_bms.temp_cell_low_dchg  = severity; break;
        case 8:  g_sim_bms.temp_relay_high     = severity; break;
        case 9:  g_sim_bms.over_chg_curr       = severity; break;
        case 10: g_sim_bms.over_dchg_curr      = severity; break;
        case 11: g_sim_bms.cell_volt_diff      = severity; break;
        case 12: g_sim_bms.low_soc             = severity; break;
        default: break;
    }
}

static bool test_bms_alm_info_raw_e005(void)
{
    printf("Running test_bms_alm_info_raw_e005...\n");
    uint8_t frame[8] = {0};

    /* In Motorola CAN layout, temp_cell_high_chg is bits 7-6 of byte 1. */
    BMS_Init();
    frame[1] = 0x80U; /* severity=2 (fault): (2 << 6) */
    BMS_FeedFrame(0U, BMS_ID_ALM_INFO, frame, 8U);

    BMS_View_t view;
    BMS_GetView(&view);
    ASSERT((view.alarm_flags & BMS_ALARM_TEMP_HIGH_CHG) != 0U,
           "ALM_INFO byte1 severity 2 must set TEMP_HIGH_CHG");

    frame[1] = 0x40U; /* warning only: (1 << 6), not an E005 fault */
    BMS_FeedFrame(0U, BMS_ID_ALM_INFO, frame, 8U);
    BMS_GetView(&view);
    ASSERT((view.alarm_flags & BMS_ALARM_TEMP_HIGH_CHG) == 0U,
           "ALM_INFO warning severity must not set E005 fault flag");
    ASSERT((view.warning_flags & BMS_ALARM_TEMP_HIGH_CHG) != 0U,
           "ALM_INFO warning severity must set E005 warning flag");

    frame[1] = 0xC0U; /* severity=3 (severe): (3 << 6) */
    BMS_FeedFrame(0U, BMS_ID_ALM_INFO, frame, 8U);
    BMS_GetView(&view);
    ASSERT((view.alarm_flags & BMS_ALARM_TEMP_HIGH_CHG) != 0U,
           "ALM_INFO byte1 severity 3 must set TEMP_HIGH_CHG");
    ASSERT(strcmp(DWIN_Alarm_GetCodeString(ALARM_BMS_TEMP_HIGH_CHG), "E005") == 0,
           "TEMP_HIGH_CHG must map to DWIN E005");
    printf("[PASS] test_bms_alm_info_raw_e005\n");
    return true;
}

static bool test_alarm_log_sequence_survives_full_ring(void)
{
    printf("Running test_alarm_log_sequence_survives_full_ring...\n");
    ASSERT(setup(NULL), "setup");
    healthy_bms(400.0f);
    ASSERT(start_running(), "controller never RUNNING");

    /* Raise all 13 BMS alarm fields, then clear them. This creates 26 edges
     * through the real alarm path without repeatedly restarting the charge
     * controller after a critical alarm stops it. */
    for (uint8_t field = 0U; field < 13U; field++) {
        set_bms_alarm_severity(field, 2U);
    }
    drive_ms(800U);
    for (uint8_t field = 0U; field < 13U; field++) {
        set_bms_alarm_severity(field, 0U);
    }
    drive_ms(800U);

    AlarmLogEntry_t log[ALARM_LOG_DEPTH];
    uint8_t count = Alarm_GetLog(log, ALARM_LOG_DEPTH);
    uint32_t full_sequence = Alarm_GetLogSequence();
    ASSERT(count == ALARM_LOG_DEPTH, "event ring must be full");
    ASSERT(full_sequence >= ALARM_LOG_DEPTH, "sequence must advance with log writes");

    uint32_t before_new_raise = full_sequence;
    set_bms_alarm_severity(4U, 2U);
    drive_ms(800U);

    count = Alarm_GetLog(log, ALARM_LOG_DEPTH);
    ASSERT(count == ALARM_LOG_DEPTH, "log count must remain saturated at ring depth");
    ASSERT(Alarm_GetLogSequence() > before_new_raise,
           "sequence must detect a write after the ring is full");
    ASSERT(log[0].event == 1U && log[0].code == (uint16_t)ALARM_BMS_TEMP_HIGH_CHG,
           "new E005 raise must be newest ring entry after wrap");

    printf("[PASS] test_alarm_log_sequence_survives_full_ring\n");
    return true;
}

static bool test_all_bms_alm_info_fields_from_pdf(void)
{
    printf("Running test_all_bms_alm_info_fields_from_pdf...\n");
    static const struct {
        uint8_t field;
        BMS_AlarmFlag_t flag;
        AlarmCode_t code;
        const char *dwin_code;
        AlarmAction_t action;
    } cases[] = {
        {0U,  BMS_ALARM_LOW_PACK_VOLT,   ALARM_BMS_LOW_PACK_VOLT,   "E001", ALARM_ACT_INFO},
        {1U,  BMS_ALARM_LOW_CELL_VOLT,   ALARM_BMS_LOW_CELL_VOLT,   "E002", ALARM_ACT_INFO},
        {2U,  BMS_ALARM_HIGH_PACK_VOLT,  ALARM_BMS_HIGH_PACK_VOLT,  "E003", ALARM_ACT_STOP},
        {3U,  BMS_ALARM_HIGH_CELL_VOLT,  ALARM_BMS_HIGH_CELL_VOLT,  "E004", ALARM_ACT_STOP},
        {4U,  BMS_ALARM_TEMP_HIGH_CHG,   ALARM_BMS_TEMP_HIGH_CHG,   "E005", ALARM_ACT_INFO},
        {5U,  BMS_ALARM_TEMP_HIGH_DCHG,  ALARM_BMS_TEMP_HIGH_DCHG,  "W001", ALARM_ACT_INFO},
        {6U,  BMS_ALARM_TEMP_LOW_CHG,    ALARM_BMS_TEMP_LOW_CHG,    "E006", ALARM_ACT_STOP},
        {7U,  BMS_ALARM_TEMP_LOW_DCHG,   ALARM_BMS_TEMP_LOW_DCHG,   "W002", ALARM_ACT_INFO},
        {8U,  BMS_ALARM_TEMP_RELAY_HIGH, ALARM_BMS_TEMP_RELAY_HIGH, "W003", ALARM_ACT_INFO},
        {9U,  BMS_ALARM_OVER_CHG_CURR,   ALARM_BMS_OVER_CHG_CURR,   "E007", ALARM_ACT_STOP},
        {10U, BMS_ALARM_OVER_DCHG_CURR,  ALARM_BMS_OVER_DCHG_CURR,  "W004", ALARM_ACT_INFO},
        {11U, BMS_ALARM_CELL_VOLT_DIFF,  ALARM_BMS_CELL_VOLT_DIFF,  "W005", ALARM_ACT_INFO},
        {12U, BMS_ALARM_LOW_SOC,         ALARM_BMS_LOW_SOC,         "W006", ALARM_ACT_INFO},
    };

    for (size_t i = 0U; i < sizeof(cases) / sizeof(cases[0]); i++) {
        ASSERT(setup(NULL), "setup");
        healthy_bms(400.0f);
        ASSERT(start_running(), "controller never RUNNING");
        set_bms_alarm_severity(cases[i].field, 2U);
        drive_ms(800U);

        BMS_View_t bms_view;
        BMS_GetView(&bms_view);
        ASSERT((bms_view.alarm_flags & cases[i].flag) != 0U,
               "PDF ALM_INFO field did not reach normalized BMS flag");
        ASSERT(alarm_active(cases[i].code) || alarm_logged_raise(cases[i].code),
               "normalized BMS flag did not reach unified alarm");
        ASSERT(strcmp(DWIN_Alarm_GetCodeString(cases[i].code), cases[i].dwin_code) == 0,
               "BMS alarm DWIN code mismatch");

        AlarmView_t alarm_view;
        Alarm_GetView(&alarm_view);
        ASSERT(alarm_view.highest_action == cases[i].action,
               "BMS alarm action mismatch");
    }

    /* Severity 1 is a BMS warning: it must be reported without being
     * promoted to a fault or stopping an active charge. */
    ASSERT(setup(NULL), "setup warning threshold");
    healthy_bms(400.0f);
    ASSERT(start_running(), "controller never RUNNING for warning threshold");
    for (uint8_t field = 0U; field < 13U; field++) set_bms_alarm_severity(field, 1U);
    drive_ms(800U);
    BMS_View_t warning_view;
    BMS_GetView(&warning_view);
    ASSERT(warning_view.alarm_flags == BMS_ALARM_NONE,
           "severity 1 ALM_INFO was incorrectly promoted to fault flags");
    ASSERT(warning_view.warning_flags == (BMS_AlarmFlag_t)((1U << 13) - 1U),
           "severity 1 ALM_INFO did not populate all warning flags");

    AlarmView_t warning_alarm;
    Alarm_GetView(&warning_alarm);
    ASSERT(warning_alarm.active_count == 13U,
           "all severity 1 BMS warnings must reach unified alarm view");
    ASSERT(warning_alarm.highest_action == ALARM_ACT_INFO,
           "severity 1 BMS warnings must remain INFO");
    ChargeCtrlView_t warning_ctrl;
    ChargeController_GetView(&warning_ctrl);
    ASSERT(warning_ctrl.state == CHARGE_CTRL_STATE_RUNNING,
           "severity 1 BMS warnings must not stop charging");

    /* Raise one of the same active bits to severity 2. The boolean alarm
     * condition remains true, so the safety action must still upgrade from
     * INFO to STOP without relying on a new active edge. */
    g_sim_bms.high_cell_volt = 2U;
    drive_ms(200U);
    BMS_GetView(&warning_view);
    ASSERT((warning_view.warning_flags & BMS_ALARM_HIGH_CELL_VOLT) == 0U,
           "severity transition must remove the old warning flag");
    ASSERT((warning_view.alarm_flags & BMS_ALARM_HIGH_CELL_VOLT) != 0U,
           "severity transition must set the fault flag");
    Alarm_GetView(&warning_alarm);
    ASSERT(warning_alarm.highest_action == ALARM_ACT_STOP,
           "warning-to-fault transition must upgrade to STOP");

    printf("[PASS] test_all_bms_alm_info_fields_from_pdf\n");
    return true;
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
    ASSERT(strcmp(DWIN_Alarm_GetCodeString(ALARM_MOD_OUTPUT_UNDER_VOLT), "W012") == 0,
           "output undervoltage warning must map to W012");
    ASSERT(strcmp(DWIN_Alarm_GetCodeString(ALARM_MOD_OUTPUT_OVER_VOLT_WARN), "W013") == 0,
           "output overvoltage warning must map to W013");

    uint8_t desc_len = 0U;
    ASSERT(DWIN_Alarm_GetDescUtf16(ALARM_MOD_FAN_FAULT, &desc_len) != NULL && desc_len > 0U,
           "fan fault description must be available");
    ASSERT(DWIN_Alarm_GetDescUtf16(ALARM_MOD_AC_OVER_VOLT, &desc_len) != NULL && desc_len > 0U,
           "AC input overvoltage description must be available");
    ASSERT(DWIN_Alarm_GetDescUtf16(ALARM_MOD_OUTPUT_UNDER_VOLT, &desc_len) != NULL && desc_len > 0U,
           "output undervoltage warning description must be available");
    ASSERT(DWIN_Alarm_GetDescUtf16(ALARM_MOD_OUTPUT_OVER_VOLT_WARN, &desc_len) != NULL && desc_len > 0U,
           "output overvoltage warning description must be available");

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

    ASSERT(setup(NULL), "setup");
    healthy_bms(400.0f);
    ASSERT(start_running(), "controller never RUNNING");
    g_sim_module.tonhe_fault_bits = (1U << 12);
    drive_ms(3000U);
    ASSERT(alarm_active(ALARM_MOD_OUTPUT_UNDER_VOLT), "W012 warning not active");
    Alarm_GetView(&view);
    ASSERT(view.highest_action == ALARM_ACT_INFO, "W012 must remain INFO over time");
    ASSERT(!alarm_active(ALARM_MOD_OVER_VOLT_OUT), "W012 must not become E012");

    ASSERT(setup(NULL), "setup");
    healthy_bms(400.0f);
    ASSERT(start_running(), "controller never RUNNING");
    g_sim_module.tonhe_fault_bits = (1U << 13);
    drive_ms(3000U);
    ASSERT(alarm_active(ALARM_MOD_OUTPUT_OVER_VOLT_WARN), "W013 warning not active");
    Alarm_GetView(&view);
    ASSERT(view.highest_action == ALARM_ACT_INFO, "W013 must remain INFO over time");
    ASSERT(!alarm_active(ALARM_MOD_OVER_VOLT_OUT), "W013 must not become E012");
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

static bool test_bms_thermal_warning_suppresses_load_lost(void)
{
    printf("Running test_bms_thermal_warning_suppresses_load_lost...\n");
    ASSERT(setup(NULL), "setup");
    healthy_bms(400.0f);
    ASSERT(start_running(), "controller never RUNNING");
    establish_load(400.0f, 40.0f);

    /* Severity 1 identifies the thermal cause but remains reporting-only. */
    set_bms_alarm_severity(4U, 1U);
    drive_ms(600U);

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    g_sim_module.voltage = cv.target_voltage_v;
    g_sim_module.current = 0.0f;
    g_sim_bms.pack_current_a = 0.0f;
    drive_ms(1500U);

    AlarmView_t view;
    Alarm_GetView(&view);
    ASSERT(alarm_active(ALARM_BMS_TEMP_HIGH_CHG),
           "severity 1 thermal warning must remain visible");
    ASSERT(view.highest_action == ALARM_ACT_INFO,
           "severity 1 thermal warning must remain INFO");
    ASSERT(!alarm_active(ALARM_DC_LOAD_LOST),
           "known BMS thermal cause must suppress latched E023");
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_RUNNING,
           "severity 1 thermal warning must not stop charging by itself");
    printf("[PASS] test_bms_thermal_warning_suppresses_load_lost\n");
    return true;
}

static bool test_bms_thermal_fault_recovers_without_e023(void)
{
    printf("Running test_bms_thermal_fault_recovers_without_e023...\n");
    ASSERT(setup(NULL), "setup");
    healthy_bms(400.0f);
    ASSERT(start_running(), "controller never RUNNING");
    establish_load(400.0f, 40.0f);

    set_bms_alarm_severity(4U, 2U);
    drive_ms(1000U);

    AlarmView_t view;
    Alarm_GetView(&view);
    ASSERT(alarm_active(ALARM_BMS_TEMP_HIGH_CHG),
           "severity 2 thermal alarm must be visible");
    ASSERT(view.highest_action == ALARM_ACT_INFO,
           "severity 2 thermal alarm must remain an INFO mirror");
    ASSERT(!alarm_active(ALARM_DC_LOAD_LOST),
           "BMS thermal fault must not create duplicate E023");

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_RUNNING,
           "severity 2 thermal alarm must keep the controller session alive");
    ASSERT(cv.inhibit != 0U && cv.applied_current_per_module_a == 0.0f,
           "severity 2 thermal alarm must inhibit current");

    set_bms_alarm_severity(4U, 0U);
    drive_ms(3500U);
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_RUNNING,
           "controller must remain RUNNING after thermal recovery");
    ASSERT(cv.inhibit == 0U,
           "thermal inhibit must clear after fresh BMS recovery");
    ASSERT(g_sim_module.actually_on,
           "module session must remain active during thermal recovery");
    printf("[PASS] test_bms_thermal_fault_recovers_without_e023\n");
    return true;
}

static bool test_controller_temperature_inhibit_suppresses_load_lost(void)
{
    printf("Running test_controller_temperature_inhibit_suppresses_load_lost...\n");
    ChargeCycleConfig_t cfg;
    ASSERT(setup(&cfg), "setup");

    cfg.temp_enabled = 1U;
    cfg.temp_delta_c = 1.0f;
    cfg.temp_1_c = 10.0f;
    cfg.temp_2_c = 20.0f;
    cfg.temp_3_c = 40.0f;
    cfg.temp_4_c = 50.0f;
    cfg.temp_5_c = 55.0f;
    cfg.temp_curr_1_c = 1.0f;
    cfg.temp_curr_2_c = 0.8f;
    cfg.temp_curr_3_c = 0.5f;
    cfg.temp_curr_4_c = 0.2f;
    ASSERT(ChargeCycleConfig_Set(&cfg), "temperature config rejected");

    healthy_bms(400.0f);
    ASSERT(start_running(), "controller never RUNNING");
    establish_load(400.0f, 40.0f);

    /* Temperature stage ABOVE_MAX is a recoverable controller inhibit. */
    g_sim_bms.max_cell_temp_c = 60.0f;
    drive_ms(600U);

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.inhibit != 0U &&
           cv.active_limit_source == CHARGE_LIMIT_SOURCE_TEMPERATURE,
           "controller must expose temperature as the active inhibit source");

    g_sim_module.voltage = cv.target_voltage_v;
    g_sim_module.current = 0.0f;
    g_sim_bms.pack_current_a = 0.0f;
    drive_ms(1500U);

    ASSERT(!alarm_active(ALARM_DC_LOAD_LOST),
           "controller thermal inhibit must suppress latched E023");
    ASSERT(!alarm_active(ALARM_DC_OUT_NOT_ESTABLISHED),
           "controller thermal inhibit must not trigger DC_OUT_NOT_ESTABLISHED");
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_RUNNING,
           "recoverable temperature inhibit must keep RUNNING state");
    ASSERT(g_sim_module.actually_on,
           "module must stay ON during stage inhibit");

    /* Temperature cools down to 50.0C (< 55.0C - 1.0C = 54.0C): auto-recovery */
    g_sim_bms.max_cell_temp_c = 50.0f;
    drive_ms(600U);

    ChargeController_GetView(&cv);
    ASSERT(cv.inhibit == 0U, "inhibit must clear upon cooling below hysteresis");
    ASSERT(cv.state == CHARGE_CTRL_STATE_RUNNING, "controller must remain RUNNING on recovery");
    ASSERT(g_sim_module.actually_on, "module must stay ON throughout recovery");

    /* During current ramp-up, simulate module current following the ramp */
    for (int step = 0; step < 20; step++) {
        drive_ms(100U);
        ChargeController_GetView(&cv);
        g_sim_module.current = cv.applied_current_per_module_a;
        g_sim_bms.pack_current_a = cv.applied_current_per_module_a;
        ASSERT(!alarm_active(ALARM_DC_LOAD_LOST),
               "LOAD_LOST must not trigger during ramp recovery");
        ASSERT(!alarm_active(ALARM_DC_OUT_NOT_ESTABLISHED),
               "DC_OUT_NOT_ESTABLISHED must not trigger during ramp recovery");
    }

    ChargeController_GetView(&cv);
    ASSERT(cv.applied_current_per_module_a > 5.0f, "current must ramp up toward target");
    ASSERT(cv.state == CHARGE_CTRL_STATE_RUNNING, "charging continues uninterrupted");

    printf("[PASS] test_controller_temperature_inhibit_suppresses_load_lost\n");
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
    ASSERT(!alarm_active(ALARM_BMS_NO_PACK_VOLTAGE) &&
           !alarm_logged_raise(ALARM_BMS_NO_PACK_VOLTAGE),
           "a valid BMS CAN alarm must not synthesize E022");
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

    /* 1. AC input drops -> both module and derived AC_UNDERVOLT trip */
    g_sim_module.tonhe_fault_bits = (1U << 0);   /* input undervoltage */
    drive_ms(1500U);

    ASSERT(alarm_logged_raise(ALARM_MOD_AC_UNDER_VOLT), "module AC-undervolt mirror missing");
    ASSERT(alarm_logged_raise(ALARM_AC_UNDERVOLT), "derived AC_UNDERVOLT missing");
    ASSERT(alarm_active(ALARM_AC_UNDERVOLT), "derived AC_UNDERVOLT must be active");

    /* 2. AC grid recovers -> both alarms auto-clear after debounce */
    g_sim_module.tonhe_fault_bits = 0;
    drive_ms(1500U);

    ASSERT(!alarm_active(ALARM_AC_UNDERVOLT), "derived AC_UNDERVOLT must clear after AC recovers");
    ASSERT(!alarm_active(ALARM_MOD_AC_UNDER_VOLT), "module AC-undervolt mirror must clear after AC recovers");
    AlarmView_t av;
    Alarm_GetView(&av);
    ASSERT(av.worst_code == ALARM_NONE, "worst_code must return to ALARM_NONE (0000)");

    printf("[PASS] test_module_ac_undervolt_mirrored_and_derived\n");
    return true;
}

static bool test_acknowledge_clears_latched(void)
{
    printf("Running test_dc_load_lost_hold_and_auto_recover_to_0000...\n");
    ASSERT(setup(NULL), "setup");
    healthy_bms(400.0f);
    ASSERT(start_running(), "controller never RUNNING");
    establish_load(400.0f, 40.0f);

    ChargeCtrlView_t cv; ChargeController_GetView(&cv);
    g_sim_module.voltage = cv.target_voltage_v;
    g_sim_module.current = 0.0f;
    g_sim_bms.pack_current_a = 0.0f;
    drive_ms(1500U);
    ASSERT(alarm_active(ALARM_DC_LOAD_LOST), "precondition: DC_LOAD_LOST active");

    /* Hold window: alarm stays visible for operator (at 1500ms into hold window) */
    drive_ms(1500U);
    ASSERT(alarm_active(ALARM_DC_LOAD_LOST), "alarm must remain visible during 3s hold window");

    /* After 3s clear window expires, alarm auto-recovers to 0000 */
    drive_ms(2000U);
    ASSERT(!alarm_active(ALARM_DC_LOAD_LOST), "alarm must auto-clear after 3s hold window");
    AlarmView_t av;
    Alarm_GetView(&av);
    ASSERT(av.active_count == 0U, "active count must be 0");
    ASSERT(av.worst_code == ALARM_NONE, "worst_code must be ALARM_NONE (0000)");

    /* Verify both RAISE and CLEAR were recorded in the event log */
    AlarmLogEntry_t log[ALARM_LOG_DEPTH];
    uint8_t n = Alarm_GetLog(log, ALARM_LOG_DEPTH);
    bool saw_raise = false;
    bool saw_clear = false;
    for (uint8_t i = 0; i < n; i++) {
        if (log[i].code == (uint16_t)ALARM_DC_LOAD_LOST) {
            if (log[i].event == 1U) saw_raise = true;
            if (log[i].event == 0U) saw_clear = true;
        }
    }
    ASSERT(saw_raise && saw_clear, "both RAISE and CLEAR must be preserved in event log");

    printf("[PASS] test_dc_load_lost_hold_and_auto_recover_to_0000\n");
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

static bool test_bms_alarm_timeout_auto_recovers_to_0000(void)
{
    printf("Running test_bms_alarm_timeout_auto_recovers_to_0000...\n");
    ASSERT(setup(NULL), "setup");
    healthy_bms(400.0f);
    g_sim_bms.alm_info_tx_enabled = false; /* ALM_INFO only transmitted when event occurs */
    ASSERT(start_running(), "controller never RUNNING");
    establish_load(400.0f, 40.0f);

    /* 1. BMS sends ALM_INFO with temp_cell_high_chg = 2 (E005 fault) */
    uint8_t frame[8] = {0};
    frame[1] = 0x80U; /* temp_cell_high_chg severity 2 */
    BMS_FeedFrame(0U, BMS_ID_ALM_INFO, frame, 8U);
    drive_ms(100U);

    BMS_View_t bms_view;
    BMS_GetView(&bms_view);
    ASSERT((bms_view.alarm_flags & BMS_ALARM_TEMP_HIGH_CHG) != 0U,
           "ALM_INFO must set TEMP_HIGH_CHG");
    AlarmView_t av;
    Alarm_GetView(&av);
    ASSERT(av.worst_code == ALARM_BMS_TEMP_HIGH_CHG, "worst_code must be E005");
    ASSERT(strcmp(DWIN_Alarm_GetCodeString(av.worst_code), "E005") == 0,
           "E005 string check");

    /* 2. Over-temperature clears: per vendor PDF §5.4, BMS stops sending 0x07F4.
     * Advance time past BMS_ALM_INFO_TIMEOUT_MS (1000ms) + 100ms debounce clear. */
    drive_ms(1200U);

    /* 3. Verify ALM_INFO timed out and alarm auto-recovered */
    BMS_GetView(&bms_view);
    ASSERT((bms_view.alarm_flags & BMS_ALARM_TEMP_HIGH_CHG) == 0U,
           "TEMP_HIGH_CHG must clear after ALM_INFO timeout");
    Alarm_GetView(&av);
    ASSERT(av.active_count == 0U, "all alarms must be cleared");
    ASSERT(av.latched_mask == 0U, "no alarms should be latched");
    ASSERT(av.worst_code == ALARM_NONE, "worst_code must be ALARM_NONE");

    /* Topbar fault code logic: if active_count == 0 and latched_mask == 0 -> "0000" */
    char topbar_code[8] = {0};
    if (av.active_count == 0U && av.latched_mask == 0U) {
        strncpy(topbar_code, "0000", sizeof(topbar_code) - 1U);
    } else {
        const char *c_str = DWIN_Alarm_GetCodeString(av.worst_code);
        strncpy(topbar_code, c_str, sizeof(topbar_code) - 1U);
    }
    ASSERT(strcmp(topbar_code, "0000") == 0, "topbar must reset to 0000");

    printf("[PASS] test_bms_alarm_timeout_auto_recovers_to_0000\n");
    return true;
}

int main(void)
{
    bool ok = true;
    ok &= test_bms_alm_info_raw_e005();
    ok &= test_alarm_log_sequence_survives_full_ring();
    ok &= test_all_bms_alm_info_fields_from_pdf();
    ok &= test_happy_path_no_alarm();
    ok &= test_module_specific_alarms_and_dwin_text();
    ok &= test_cv_taper_no_false_load_lost();
    ok &= test_dc_load_lost_hot_unplug();
    ok &= test_bms_thermal_warning_suppresses_load_lost();
    ok &= test_bms_thermal_fault_recovers_without_e023();
    ok &= test_controller_temperature_inhibit_suppresses_load_lost();
    ok &= test_dc_out_not_established();
    ok &= test_bms_comm_lost_mid_charge();
    ok &= test_bms_no_pack_voltage();
    ok &= test_bms_critical_alarm_mirrored();
    ok &= test_module_ac_undervolt_mirrored_and_derived();
    ok &= test_acknowledge_clears_latched();
    ok &= test_start_with_no_module_or_bms_reports_fault_code();
    ok &= test_bms_alarm_timeout_auto_recovers_to_0000();

    if (ok) { printf("\nALL TESTS PASSED.\n"); return 0; }
    printf("\nSOME TESTS FAILED.\n");
    return 1;
}
