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

static SimDriverKind_t g_sim_driver_kind = SIM_DRV_TONHE;

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
    sim_module_tick(g_sim_driver_kind, mock_tick);
    CHG_LIB_Process(mock_tick);
    BMS_Process(mock_tick);
    ChargeController_Process(mock_tick);
    Alarm_Process(mock_tick);
}

/* Advance the module/BMS/Alarm snapshot without advancing the controller.
 * This is intentional for READY/PRECHARGE coverage: those states normally
 * transition on the next controller tick, but W010 must still derive from
 * the state that Alarm actually receives. */
static void drive_alarm_only(uint32_t step_ms)
{
    mock_tick += step_ms;
    sim_bms_tick(mock_tick);
    sim_module_tick(g_sim_driver_kind, mock_tick);
    CHG_LIB_Process(mock_tick);
    BMS_Process(mock_tick);
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
static bool setup_variant(uint8_t module_type, uint8_t module_count,
                          ChargeCycleConfig_t *cfg_out);
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

    frame[1] = 0x40U; /* severity=1: now sets fault flag per spec */
    BMS_FeedFrame(0U, BMS_ID_ALM_INFO, frame, 8U);
    BMS_GetView(&view);
    ASSERT((view.alarm_flags & BMS_ALARM_TEMP_HIGH_CHG) != 0U,
           "ALM_INFO severity 1 must set E005 fault flag");
    ASSERT((view.warning_flags & BMS_ALARM_TEMP_HIGH_CHG) != 0U,
           "ALM_INFO severity 1 must set E005 warning flag");

    frame[1] = 0x00U; /* severity=0: cleared */
    BMS_FeedFrame(0U, BMS_ID_ALM_INFO, frame, 8U);
    BMS_GetView(&view);
    ASSERT((view.alarm_flags & BMS_ALARM_TEMP_HIGH_CHG) == 0U,
           "ALM_INFO severity 0 must clear E005 fault flag");
    ASSERT((view.warning_flags & BMS_ALARM_TEMP_HIGH_CHG) == 0U,
           "ALM_INFO severity 0 must clear E005 warning flag");

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

    /* Raise all BMS alarm fields, then clear them. Repeat twice so that 44 edges
     * easily exceed the 24-entry ring buffer depth (ALARM_LOG_DEPTH). This creates
     * edges through the real alarm path without repeatedly restarting the charge
     * controller after a critical alarm stops it. */
    for (int pass = 0; pass < 2; pass++) {
        for (uint8_t field = 0U; field < 13U; field++) {
            set_bms_alarm_severity(field, 2U);
        }
        drive_ms(800U);
        for (uint8_t field = 0U; field < 13U; field++) {
            set_bms_alarm_severity(field, 0U);
        }
        drive_ms(800U);
    }

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
        {0U,  BMS_ALARM_LOW_PACK_VOLT,   ALARM_BMS_LOW_PACK_VOLT,   "E001", ALARM_ACT_STOP},
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

    /* Severity 1 is now a BMS fault per spec: it must populate both
     * alarm_flags and warning_flags, triggering STOP action on fault fields. */
    ASSERT(setup(NULL), "setup warning threshold");
    healthy_bms(400.0f);
    ASSERT(start_running(), "controller never RUNNING for warning threshold");
    for (uint8_t field = 0U; field < 13U; field++) set_bms_alarm_severity(field, 1U);
    drive_ms(800U);
    BMS_View_t warning_view;
    BMS_GetView(&warning_view);
    ASSERT(warning_view.alarm_flags == (BMS_AlarmFlag_t)((1U << 13) - 1U),
           "severity 1 ALM_INFO must populate all alarm fault flags");
    ASSERT(warning_view.warning_flags == (BMS_AlarmFlag_t)((1U << 13) - 1U),
           "severity 1 ALM_INFO must populate all warning flags");

    AlarmView_t warning_alarm;
    Alarm_GetView(&warning_alarm);
    ASSERT(warning_alarm.active_count == 11U,
           "all active severity 1 BMS alarms must reach unified alarm view");
    ASSERT(warning_alarm.highest_action == ALARM_ACT_STOP,
           "severity 1 BMS alarms with stop action must trigger ALARM_ACT_STOP");

    /* Clearing all fields (severity 0) clears both flags */
    for (uint8_t field = 0U; field < 13U; field++) set_bms_alarm_severity(field, 0U);
    drive_ms(800U);
    BMS_GetView(&warning_view);
    ASSERT(warning_view.alarm_flags == BMS_ALARM_NONE,
           "severity 0 must clear all fault flags");
    ASSERT(warning_view.warning_flags == BMS_ALARM_NONE,
           "severity 0 must clear all warning flags");

    printf("[PASS] test_all_bms_alm_info_fields_from_pdf\n");
    return true;
}

static bool setup_variant(uint8_t module_type, uint8_t module_count,
                          ChargeCycleConfig_t *cfg_out)
{
    static bool registered = false;
    if (!registered) {
        CHG_LIB_RegisterDriver(CHG_LIB_DRV_MAXWELL, CHG_LIB_MaxwellDriverOps());
        CHG_LIB_RegisterDriver(CHG_LIB_DRV_LIANMING, CHG_LIB_LianmingDriverOps());
        CHG_LIB_RegisterDriver(CHG_LIB_DRV_TONHE, CHG_LIB_TonheDriverOps());
        registered = true;
    }
    mock_tick = 0;
    g_sim_driver_kind = (module_type == CHARGE_MODULE_TYPE_MAXWELL)
        ? SIM_DRV_MAXWELL : SIM_DRV_TONHE;
    sim_install_backend(g_sim_driver_kind);
    if (module_count > 1U && g_sim_driver_kind == SIM_DRV_MAXWELL) {
        sim_module_reset_n(module_count, 1U, 0U);
    } else {
        sim_module_reset(&g_sim_module, 1U, 0U);
    }
    for (uint8_t i = 0U; i < module_count && i < SIM_MAX_MODULES; i++) {
        g_sim_modules[i].current_override = true; /* tests drive current directly */
        g_sim_modules[i].rated_current = 100.0f;
        if (g_sim_driver_kind == SIM_DRV_MAXWELL) {
            g_sim_modules[i].voltage = 400.0f;
            g_sim_modules[i].voltage_override = true;
        }
    }
    sim_bms_reset(&g_sim_bms);

    BMS_Init();
    ChargeCycleConfig_Init();
    ChargeController_Init();
    Alarm_Init();

    ChargeCycleConfig_t cfg;
    ChargeCycleConfig_GetDefaults(&cfg);
    cfg.module_type = module_type;
    cfg.source_module_count = module_count;
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

static bool setup(ChargeCycleConfig_t *cfg_out)
{
    return setup_variant(CHARGE_MODULE_TYPE_TONHE, 1U, cfg_out);
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

static void set_maxwell_load(float module0_a, float module1_a)
{
    g_sim_modules[0].actually_on = true;
    g_sim_modules[1].actually_on = true;
    g_sim_modules[0].voltage = 400.0f;
    g_sim_modules[1].voltage = 400.0f;
    g_sim_modules[0].current = module0_a;
    g_sim_modules[1].current = module1_a;
    g_sim_bms.pack_current_a = module0_a + module1_a;
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

    /* TonHe bits 12 and 13 (previously W012/W013) must be ignored now */
    ASSERT(setup(NULL), "setup");
    healthy_bms(400.0f);
    ASSERT(start_running(), "controller never RUNNING");
    g_sim_module.tonhe_fault_bits = (1U << 12) | (1U << 13);
    drive_ms(800U);
    Alarm_GetView(&view);
    ASSERT(view.active_count == 0, "W012/W013 removal: bits 12/13 must not raise alarms");

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

    /* Severity 1 is in alarm_flags, but this particular thermal alarm keeps
     * INFO action because the controller owns the thermal inhibit/recovery. */
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

static bool test_bms_thermal_trip_limit_halts_on_4th(void)
{
    printf("Running test_bms_thermal_trip_limit_halts_on_4th...\n");
    ASSERT(setup(NULL), "setup");
    healthy_bms(400.0f);
    ASSERT(start_running(), "controller never RUNNING");
    establish_load(400.0f, 40.0f);

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.bms_temp_trip_count == 0U, "initial trip count must be 0");

    /* Trip 1, 2, 3: Must inhibit (0A) and auto-recover */
    for (uint8_t trip = 1U; trip <= 3U; trip++) {
        /* Over-temperature trips */
        set_bms_alarm_severity(4U, 2U);
        drive_ms(1000U);

        ChargeController_GetView(&cv);
        ASSERT(cv.state == CHARGE_CTRL_STATE_RUNNING, "controller must stay RUNNING during recoverable trip");
        ASSERT(cv.inhibit != 0U && cv.applied_current_per_module_a == 0.0f, "current must be clamped to 0A");
        ASSERT(cv.bms_temp_trip_count == trip, "trip count mismatch");

        /* Cool down and auto-recover */
        set_bms_alarm_severity(4U, 0U);
        drive_ms(3500U);

        ChargeController_GetView(&cv);
        ASSERT(cv.state == CHARGE_CTRL_STATE_RUNNING, "controller must recover to RUNNING");
        ASSERT(cv.inhibit == 0U, "inhibit must clear on recovery");
        ASSERT(cv.bms_temp_trip_count == trip, "trip count must persist across recoveries");

        /* Re-establish load for next cycle */
        establish_load(400.0f, 40.0f);
    }

    /* Trip 4: Must halt charge completely (STATE_FAULT), not auto-recover */
    set_bms_alarm_severity(4U, 2U);
    drive_ms(1000U);

    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_FAULT, "controller must enter FAULT state on 4th trip");
    ASSERT(cv.bms_temp_trip_count == 4U, "trip count must be 4 on 4th trip");
    ASSERT(cv.stop_reason == CHARGE_STOP_BMS_ALARM, "stop_reason must be BMS_ALARM");

    /* Module stops and current settles to 0A -> contactor opens safely */
    g_sim_module.current = 0.0f;
    g_sim_bms.pack_current_a = 0.0f;
    drive_ms(200U);
    ChargeController_GetView(&cv);
    ASSERT(!cv.relay_should_close, "contactor must open once current settles");

    /* Even if BMS cools down, controller must NOT auto-recover from FAULT */
    set_bms_alarm_severity(4U, 0U);
    drive_ms(5000U);

    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_FAULT, "controller must remain in FAULT even after cooling down");

    /* START and STOP while in FAULT must be rejected and not clear fault */
    ASSERT(!ChargeController_Start(CHARGE_CTRL_OWNER_PC, false, mock_tick),
           "START while in FAULT must be rejected");
    ChargeController_Stop(mock_tick);
    drive_ms(200U);
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_FAULT, "STOP must not clear FAULT");
    ASSERT(cv.bms_temp_trip_count == 4U, "STOP must not reset trip count");

    /* Reset clears fault and resets trip count to 0 */
    ASSERT(ChargeController_ResetFaultIfSafe(mock_tick), "reset fault should succeed once BMS is healthy");
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_IDLE, "controller must return to IDLE after reset");
    ASSERT(cv.bms_temp_trip_count == 0U, "trip count must reset to 0 after fault clear");

    printf("[PASS] test_bms_thermal_trip_limit_halts_on_4th\n");
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
    drive_ms(3500U);

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

static bool test_stage_thermal_trip_limit_halts_on_4th(void)
{
    printf("Running test_stage_thermal_trip_limit_halts_on_4th...\n");
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

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.bms_temp_trip_count == 0U, "initial trip count must be 0");

    /* Trip 1, 2, 3: Over-temperature stage (60.0C >= temp_5_c 55.0C) inhibits to 0A and recovers */
    for (uint8_t trip = 1U; trip <= 3U; trip++) {
        g_sim_bms.max_cell_temp_c = 60.0f;
        drive_ms(1000U);

        ChargeController_GetView(&cv);
        ASSERT(cv.state == CHARGE_CTRL_STATE_RUNNING, "controller must stay RUNNING during recoverable stage trip");
        ASSERT(cv.inhibit != 0U && cv.applied_current_per_module_a == 0.0f, "current must be clamped to 0A");
        ASSERT(cv.bms_temp_trip_count == trip, "trip count mismatch for stage overtemp");

        /* Cool down to 50.0C (< 55.0C - 1.0C = 54.0C) and wait for recovery confirmation (3000ms) */
        g_sim_bms.max_cell_temp_c = 50.0f;
        drive_ms(3500U);

        ChargeController_GetView(&cv);
        ASSERT(cv.state == CHARGE_CTRL_STATE_RUNNING, "controller must recover to RUNNING");
        ASSERT(cv.inhibit == 0U, "inhibit must clear on recovery");
        ASSERT(cv.bms_temp_trip_count == trip, "trip count must persist across recoveries");

        /* Re-establish load for next cycle */
        establish_load(400.0f, 40.0f);
    }

    /* Trip 4: Must halt charge completely (STATE_FAULT), not auto-recover */
    g_sim_bms.max_cell_temp_c = 60.0f;
    drive_ms(1000U);

    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_FAULT, "controller must enter FAULT state on 4th stage trip");
    ASSERT(cv.bms_temp_trip_count == 4U, "trip count must be 4 on 4th stage trip");
    ASSERT(cv.stop_reason == CHARGE_STOP_BMS_ALARM, "stop_reason must be BMS_ALARM");

    /* Module stops and current settles to 0A -> contactor opens safely */
    g_sim_module.current = 0.0f;
    g_sim_bms.pack_current_a = 0.0f;
    drive_ms(200U);
    ChargeController_GetView(&cv);
    ASSERT(!cv.relay_should_close, "contactor must open once current settles");

    /* Even if battery cools down, controller must NOT auto-recover from FAULT */
    g_sim_bms.max_cell_temp_c = 40.0f;
    drive_ms(5000U);

    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_FAULT, "controller must remain in FAULT even after cooling down");

    /* START and STOP while in FAULT must be rejected and not clear fault */
    ASSERT(!ChargeController_Start(CHARGE_CTRL_OWNER_PC, false, mock_tick),
           "START while in FAULT must be rejected");
    ChargeController_Stop(mock_tick);
    drive_ms(200U);
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_FAULT, "STOP must not clear FAULT");
    ASSERT(cv.bms_temp_trip_count == 4U, "STOP must not reset trip count");

    /* While still hot, reset must be rejected */
    g_sim_bms.max_cell_temp_c = 60.0f;
    drive_ms(600U);
    ASSERT(!ChargeController_ResetFaultIfSafe(mock_tick), "reset fault must fail while temperature is high");

    /* Once cooled down, reset clears fault and resets trip count to 0 */
    g_sim_bms.max_cell_temp_c = 40.0f;
    drive_ms(600U);
    ASSERT(ChargeController_ResetFaultIfSafe(mock_tick), "reset fault should succeed once temperature is safe");
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_IDLE, "controller must return to IDLE after reset");
    ASSERT(cv.bms_temp_trip_count == 0U, "trip count must reset to 0 after fault clear");

    printf("[PASS] test_stage_thermal_trip_limit_halts_on_4th\n");
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

static bool test_dc_out_summary_two_module_threshold(void)
{
    printf("Running test_dc_out_summary_two_module_threshold...\n");

    /* Exactly 2.0 A across two online modules is the accepted boundary and
     * must not be classified as output-not-established. */
    ASSERT(setup_variant(CHARGE_MODULE_TYPE_MAXWELL, 2U, NULL), "setup 2-module boundary");
    healthy_bms(400.0f);
    ASSERT(start_running(), "2-module controller never RUNNING");
    set_maxwell_load(1.0f, 1.0f);
    drive_ms(1200U); /* refresh the post-start voltage read used to arm relay */
    drive_ms(5200U);

    CHG_LIB_SystemSummary_t summary;
    CHG_LIB_GetSystemSummary(&summary);
    ASSERT(fabsf(summary.total_current - 2.0f) < 0.01f,
           "two 1A online modules must produce a 2A fresh summary");
    ASSERT(!alarm_active(ALARM_DC_OUT_NOT_ESTABLISHED),
           "exactly 2A must not raise E024");

    /* A total below 2.0 A must take the same path and raise E024. */
    ASSERT(setup_variant(CHARGE_MODULE_TYPE_MAXWELL, 2U, NULL), "setup 2-module below threshold");
    healthy_bms(400.0f);
    ASSERT(start_running(), "2-module controller never RUNNING below threshold");
    set_maxwell_load(0.9f, 1.0f);
    drive_ms(1200U); /* refresh the post-start voltage read used to arm relay */
    drive_ms(5200U);

    CHG_LIB_GetSystemSummary(&summary);
    ASSERT(fabsf(summary.total_current - 1.9f) < 0.01f,
           "two online modules below threshold must produce a 1.9A summary");
    ASSERT(alarm_active(ALARM_DC_OUT_NOT_ESTABLISHED),
           "current below 2A must raise E024");

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.stop_reason == CHARGE_STOP_PROTECTION,
           "E024 must use protection stop reason");

    printf("[PASS] test_dc_out_summary_two_module_threshold\n");
    return true;
}

static bool test_manual_stop_reason(void)
{
    printf("Running test_manual_stop_reason...\n");
    ASSERT(setup(NULL), "setup");
    healthy_bms(400.0f);
    ASSERT(start_running(), "controller never RUNNING");

    ChargeController_Stop(mock_tick);

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.stop_reason == CHARGE_STOP_USER_COMMAND,
           "manual Stop must record USER_COMMAND");
    ASSERT(cv.state == CHARGE_CTRL_STATE_STOPPING,
           "manual Stop from RUNNING must enter STOPPING");

    printf("[PASS] test_manual_stop_reason\n");
    return true;
}

static bool test_e030_uses_fresh_multi_module_summary(void)
{
    printf("Running test_e030_uses_fresh_multi_module_summary...\n");

    /* Module 0 will become offline while retaining a deliberately large
     * stale current/voltage. Module 1 is the only source that may satisfy the
     * E030 current and voltage conditions. */
    ChargeCycleConfig_t cfg;
    ASSERT(setup_variant(CHARGE_MODULE_TYPE_MAXWELL, 2U, &cfg),
           "setup E030 stale-data rejection");
    /* Keep E030 disabled while preparing the deliberately stale online
     * values; otherwise the stale module would correctly trip E030 before it
     * becomes offline. */
    cfg.protect_jack_charge_enabled = 0U;
    cfg.protect_jack_charge_delta_v = 2.0f;
    cfg.protect_jack_charge_delay_s = 1U;
    ASSERT(ChargeCycleConfig_Set(&cfg), "E030 config set failed");
    healthy_bms(400.0f);
    ASSERT(start_running(), "2-module controller never RUNNING for E030");
    set_maxwell_load(100.0f, 1.5f);
    g_sim_modules[0].voltage = 450.0f; /* stale value must be excluded */
    g_sim_modules[1].voltage = 403.0f;
    drive_ms(4000U); /* refresh voltage and current reads used by E030 */

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.relay_should_close != 0U, "relay must be latched before E030 test");

    g_sim_modules[0].silent = true;
    drive_ms(10300U);

    CHG_LIB_ModuleView_t stale_view;
    CHG_LIB_SystemSummary_t summary;
    ASSERT(CHG_LIB_GetModuleView(0U, &stale_view) && !stale_view.online,
           "module 0 must be offline");
    CHG_LIB_GetSystemSummary(&summary);
    ASSERT(fabsf(summary.total_current - 1.5f) < 0.01f,
           "offline module current must not satisfy E030 current threshold");
    ASSERT(fabsf(summary.voltage - 403.0f) < 0.01f,
           "offline module voltage must not replace online module voltage");

    cfg.protect_jack_charge_enabled = 1U;
    cfg.protect_jack_charge_delta_v = 2.0f;
    cfg.protect_jack_charge_delay_s = 1U;
    ASSERT(ChargeCycleConfig_Set(&cfg), "E030 stale-data config enable failed");
    drive_ms(1200U);
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_RUNNING,
           "E030 must not trip from stale module current");
    ASSERT((cv.fault_flags & CHARGE_CTRL_FAULT_PROTECT_JACK_V) == 0U,
           "stale module data must not trigger E030");

    printf("[PASS] test_e030_uses_fresh_multi_module_summary\n");
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

static bool test_bms_low_pack_voltage_with_vmin(void)
{
    printf("Running test_bms_low_pack_voltage_with_vmin...\n");
    ChargeCycleConfig_t cfg;
    ASSERT(setup(&cfg), "setup");
    /* In setup: vmax_v = 500.0f, vmin_v = 300.0f.
     * Floor 0.5 * Vmax = 250.0f.
     * Start running with healthy voltage (400V). */
    healthy_bms(400.0f);
    ASSERT(start_running(), "controller never RUNNING");

    /* Drop pack voltage into [250V, 300V), e.g. 270.0V. */
    g_sim_bms.pack_voltage_v = 270.0f;
    drive_ms(200U);

    ASSERT(alarm_logged_raise(ALARM_BMS_LOW_PACK_VOLT), "ALARM_BMS_LOW_PACK_VOLT (E001) never raised");
    ChargeCtrlView_t cv; ChargeController_GetView(&cv);
    ASSERT(cv.state != CHARGE_CTRL_STATE_RUNNING, "controller should have stopped on low pack voltage");
    printf("[PASS] test_bms_low_pack_voltage_with_vmin\n");
    return true;
}

static bool test_bms_low_pack_voltage_precharge_bypassed(void)
{
    printf("Running test_bms_low_pack_voltage_precharge_bypassed...\n");
    ChargeCycleConfig_t cfg;
    ASSERT(setup(&cfg), "setup");
    /* In setup: vmax_v = 500.0f, vmin_v = 300.0f.
     * Floor 0.5 * Vmax = 250.0f.
     * Set pack voltage in [250V, 300V), e.g. 270.0V. */
    healthy_bms(270.0f);
    drive_ms(1500U);
    ASSERT(ChargeController_StartPrecharge(CHARGE_CTRL_OWNER_PC, mock_tick),
           "precharge must start");
    drive_ms(500U);

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_PRECHARGE, "controller must be in PRECHARGE");
    ASSERT(!alarm_active(ALARM_BMS_LOW_PACK_VOLT), "E001 must not trip in PRECHARGE mode");

    ChargeController_Stop(mock_tick);
    drive_ms(500U);
    printf("[PASS] test_bms_low_pack_voltage_precharge_bypassed\n");
    return true;
}

static bool test_config_temp_limit_halts_on_4th(void)
{
    printf("Running test_config_temp_limit_halts_on_4th...\n");
    ChargeCycleConfig_t cfg;
    ASSERT(setup(&cfg), "setup");
    cfg.temp_limit_c = 60.0f;
    ASSERT(ChargeCycleConfig_Set(&cfg), "config rejected");

    healthy_bms(400.0f);
    ASSERT(start_running(), "controller never RUNNING");
    establish_load(400.0f, 40.0f);

    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.bms_temp_trip_count == 0U, "initial trip count must be 0");

    /* Trip 1, 2, 3: Must inhibit (0A) and auto-recover */
    for (uint8_t trip = 1U; trip <= 3U; trip++) {
        /* Exceed temp_limit_c (62.0C > 60.0C) without BMS ALM_INFO overtemp flag */
        g_sim_bms.max_cell_temp_c = 62.0f;
        drive_ms(1000U);

        ChargeController_GetView(&cv);
        ASSERT(cv.state == CHARGE_CTRL_STATE_RUNNING, "controller must stay RUNNING during recoverable trip");
        ASSERT(cv.inhibit != 0U && cv.applied_current_per_module_a == 0.0f, "current must be clamped to 0A");
        ASSERT(cv.bms_temp_trip_count == trip, "trip count mismatch");

        /* Cool down below (60.0 - 5.0 = 55.0C), e.g. 54.0C and auto-recover */
        g_sim_bms.max_cell_temp_c = 54.0f;
        drive_ms(3500U);

        ChargeController_GetView(&cv);
        ASSERT(cv.state == CHARGE_CTRL_STATE_RUNNING, "controller must recover to RUNNING");
        ASSERT(cv.inhibit == 0U, "inhibit must clear on recovery");
        ASSERT(cv.bms_temp_trip_count == trip, "trip count must persist across recoveries");

        /* Re-establish load for next cycle */
        establish_load(400.0f, 40.0f);
    }

    /* Trip 4: Must halt charge completely (STATE_FAULT), not auto-recover */
    g_sim_bms.max_cell_temp_c = 62.0f;
    drive_ms(1000U);

    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_FAULT, "controller must enter FAULT state on 4th trip");
    ASSERT(cv.bms_temp_trip_count == 4U, "trip count must be 4 on 4th trip");
    ASSERT(cv.stop_reason == CHARGE_STOP_BMS_ALARM, "stop_reason must be BMS_ALARM");

    printf("[PASS] test_config_temp_limit_halts_on_4th\n");
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

    /* 1. Transient AC undervoltage (e.g. capacitor discharge when AC is turned off, < 5000 ms)
     * must NOT trip W011. */
    g_sim_module.tonhe_fault_bits = (1U << 0);   /* input undervoltage */
    drive_ms(2500U);
    ASSERT(!alarm_active(ALARM_MOD_AC_UNDER_VOLT), "transient AC undervolt < 5s must NOT trip W011");

    /* Continuing past 5000 ms debounce -> now trips W011 (INFO only) */
    drive_ms(3000U); /* total 5500 ms */
    ASSERT(alarm_logged_raise(ALARM_MOD_AC_UNDER_VOLT), "persistent AC undervolt >= 5s must trip W011");
    ASSERT(alarm_active(ALARM_MOD_AC_UNDER_VOLT), "module AC-undervolt mirror must be active");

    /* Controller must remain RUNNING since E026 was removed */
    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_RUNNING, "AC under-voltage warning must not stop charge");

    /* 2. AC grid recovers -> module alarm auto-clears after debounce */
    g_sim_module.tonhe_fault_bits = 0;
    drive_ms(1500U);

    ASSERT(!alarm_active(ALARM_MOD_AC_UNDER_VOLT), "module AC-undervolt mirror must clear after AC recovers");
    AlarmView_t av;
    Alarm_GetView(&av);
    ASSERT(av.worst_code == ALARM_NONE, "worst_code must return to ALARM_NONE (0000)");

    /* 3. Both TonHe phase-loss representations normalize to E015. */
    g_sim_module.tonhe_fault_bits = (1U << 1);
    drive_ms(200U);
    ASSERT(alarm_active(ALARM_MOD_PFC_FAULT),
           "TonHe fault_bits bit 1 phase loss must raise E015");
    ChargeController_GetView(&cv);
    ASSERT(cv.stop_reason == CHARGE_STOP_PROTECTION,
           "fault_bits phase loss STOP must retain CHARGE_STOP_PROTECTION");

    /* Phase loss is a protection stop, so use a fresh session for the second
     * wire representation rather than trying to recover a stopped session. */
    ASSERT(setup(NULL), "setup pfc phase loss");
    healthy_bms(400.0f);
    ASSERT(start_running(), "controller never RUNNING for pfc phase loss");
    g_sim_module.tonhe_pfc_bits = (1U << 6);
    drive_ms(200U);
    ASSERT(alarm_active(ALARM_MOD_PFC_FAULT),
           "TonHe pfc_bits bit 6 phase loss must raise E015");
    ChargeController_GetView(&cv);
    ASSERT(cv.stop_reason == CHARGE_STOP_PROTECTION,
           "pfc_bits phase loss STOP must retain CHARGE_STOP_PROTECTION");

    printf("[PASS] test_module_ac_undervolt_mirrored_and_derived\n");
    return true;
}

static bool test_module_offline_uses_fresh_snapshot(void)
{
    printf("Running test_module_offline_uses_fresh_snapshot...\n");

    /* An offline module in IDLE is a transport fact, not an active-session
     * alarm. */
    ASSERT(setup(NULL), "setup idle offline");
    g_sim_module.silent = true;
    drive_ms(200U);
    ASSERT(!alarm_active(ALARM_MOD_COMM_FAIL),
           "offline module in IDLE must not raise W010");

    ASSERT(setup(NULL), "setup active offline");
    healthy_bms(400.0f);
    ASSERT(start_running(), "controller never RUNNING");
    establish_load(400.0f, 40.0f);

    /* Remove all frames. The alarm snapshot must derive W010 from the
     * driver's online state, and the summary must no longer expose stale
     * electrical data. */
    g_sim_module.silent = true;
    drive_ms(10300U);

    CHG_LIB_ModuleView_t mv;
    CHG_LIB_SystemSummary_t summary;
    ASSERT(CHG_LIB_GetModuleView(0U, &mv), "module view must be available");
    CHG_LIB_GetSystemSummary(&summary);
    ASSERT(!mv.online, "module must be offline after communication timeout");
    ASSERT(alarm_active(ALARM_MOD_COMM_FAIL),
           "offline module in RUNNING must raise W010");
    ASSERT(fabsf(summary.total_current) < 0.01f &&
           fabsf(summary.total_power_in) < 0.01f &&
           fabsf(summary.voltage) < 0.01f,
           "offline module must not contribute stale electrical summary");

    g_sim_module.silent = false;
    g_sim_module.tonhe_fault_bits = 0U;
    drive_ms(220U);
    ASSERT(!alarm_active(ALARM_MOD_COMM_FAIL),
           "W010 must clear 200ms after a valid recovery frame");

    printf("[PASS] test_module_offline_uses_fresh_snapshot\n");
    return true;
}

static bool test_w010_active_states_and_exact_recovery_debounce(void)
{
    printf("Running test_w010_active_states_and_exact_recovery_debounce...\n");

    /* READY is a real active-session state even though the normal controller
     * loop advances it on the next tick. Hold that state while the module
     * watchdog expires so Alarm evaluates the intended boundary directly. */
    ASSERT(setup(NULL), "setup READY W010");
    healthy_bms(400.0f);
    drive_ms(1500U);
    ASSERT(ChargeController_Start(CHARGE_CTRL_OWNER_PC, false, mock_tick),
           "start must enter READY");
    ChargeCtrlView_t cv;
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_READY, "controller must be READY before first process tick");
    g_sim_module.silent = true;
    for (uint32_t elapsed = 0U; elapsed < 10020U; elapsed += 20U) {
        drive_alarm_only(20U);
    }
    ASSERT(alarm_active(ALARM_MOD_COMM_FAIL), "READY + offline module must raise W010");

    /* PRECHARGE is also an active session and must use the same W010 rule. */
    ASSERT(setup(NULL), "setup PRECHARGE W010");
    healthy_bms(400.0f);
    drive_ms(1500U);
    ASSERT(ChargeController_StartPrecharge(CHARGE_CTRL_OWNER_PC, mock_tick),
           "precharge must start");
    ChargeController_GetView(&cv);
    ASSERT(cv.state == CHARGE_CTRL_STATE_PRECHARGE, "controller must be PRECHARGE");
    g_sim_module.silent = true;
    for (uint32_t elapsed = 0U; elapsed < 10020U; elapsed += 20U) {
        drive_alarm_only(20U);
    }
    ASSERT(alarm_active(ALARM_MOD_COMM_FAIL), "PRECHARGE + offline module must raise W010");

    /* A valid online frame carrying the real TonHe COMM_FAIL bit is distinct
     * from transport-offline and must still mirror W010. */
    ASSERT(setup(NULL), "setup online COMM_FAIL W010");
    healthy_bms(400.0f);
    ASSERT(start_running(), "controller never RUNNING for online COMM_FAIL");
    g_sim_module.tonhe_fault_bits = (uint16_t)(1U << 9);
    drive_step(20U);
    CHG_LIB_ModuleView_t mv;
    ASSERT(CHG_LIB_GetModuleView(0U, &mv) && mv.online,
           "COMM_FAIL frame must remain an online valid snapshot");
    ASSERT(alarm_active(ALARM_MOD_COMM_FAIL),
           "online valid COMM_FAIL must raise W010");

    /* Clear the driver fault before checking the exact 200ms Alarm debounce. */
    g_sim_module.tonhe_fault_bits = 0U;
    drive_ms(600U); /* TonHe fault recovery requires a clean read streak. */
    ASSERT(!alarm_active(ALARM_MOD_COMM_FAIL), "online COMM_FAIL must clear after recovery");

    ASSERT(setup(NULL), "setup exact W010 debounce");
    healthy_bms(400.0f);
    ASSERT(start_running(), "controller never RUNNING for exact debounce");
    g_sim_module.silent = true;
    drive_ms(10300U);
    ASSERT(alarm_active(ALARM_MOD_COMM_FAIL), "offline module must raise W010");

    g_sim_module.silent = false;
    drive_alarm_only(20U); /* first clean frame starts the Alarm clear timer */
    drive_alarm_only(199U);
    ASSERT(alarm_active(ALARM_MOD_COMM_FAIL), "W010 must remain active at 199ms recovery");
    drive_alarm_only(1U);
    ASSERT(!alarm_active(ALARM_MOD_COMM_FAIL), "W010 must clear at 200ms recovery");

    /* Repeat the same recovery boundary across uint32_t tick wrap. */
    ASSERT(setup(NULL), "setup W010 tick wrap");
    healthy_bms(400.0f);
    ASSERT(start_running(), "controller never RUNNING for tick wrap");
    mock_tick = UINT32_MAX - 100U;
    drive_alarm_only(20U); /* refresh a valid frame immediately before wrap */
    g_sim_module.silent = true;
    drive_alarm_only(10020U); /* timeout crosses 0U */
    ASSERT(alarm_active(ALARM_MOD_COMM_FAIL), "W010 must raise across tick wrap");

    g_sim_module.silent = false;
    drive_alarm_only(20U); /* first clean frame starts the Alarm clear timer */
    drive_alarm_only(199U);
    ASSERT(alarm_active(ALARM_MOD_COMM_FAIL), "wrapped W010 recovery must hold at 199ms");
    drive_alarm_only(1U);
    ASSERT(!alarm_active(ALARM_MOD_COMM_FAIL), "wrapped W010 recovery must clear at 200ms");

    printf("[PASS] test_w010_active_states_and_exact_recovery_debounce\n");
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
     * Advance time past BMS_ALM_INFO_TIMEOUT_MS (1000ms) + 3000ms recovery window + debounce clear. */
    drive_ms(4500U);

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

static void feed_chg_request(float volt_v, float curr_a)
{
    uint16_t volt_req = (uint16_t)(volt_v * 10.0f);
    uint16_t curr_req = (uint16_t)(curr_a * 10.0f);
    uint8_t d[8] = {0};
    d[0] = (uint8_t)(volt_req >> 8);
    d[1] = (uint8_t)volt_req;
    d[2] = (uint8_t)(curr_req >> 8);
    d[3] = (uint8_t)curr_req;
    BMS_FeedFrame(0x1806E5F4UL, 0, d, 8);
}

static bool test_bms_volt_mismatch_e032(void)
{
    printf("Running test_bms_volt_mismatch_e032...\n");
    ASSERT(strcmp(DWIN_Alarm_GetCodeString(ALARM_BMS_VOLT_MISMATCH), "E032") == 0,
           "voltage mismatch must map to E032");

    uint8_t desc_len = 0U;
    ASSERT(DWIN_Alarm_GetDescUtf16(ALARM_BMS_VOLT_MISMATCH, &desc_len) != NULL && desc_len > 0U,
           "voltage mismatch description must be available");

    ASSERT(setup(NULL), "setup");
    healthy_bms(400.0f);
    /* By default healthy_bms sets chg_volt_request_v = 500.0f and cfg.vmax_v = 500.0f */
    feed_chg_request(500.0f, 50.0f);
    drive_ms(500U);
    ASSERT(!alarm_active(ALARM_BMS_VOLT_MISMATCH), "matching voltage must not trip E032");

    /* Difference <= 2.0V (e.g. 501.5V vs 500.0V -> 1.5V) must NOT trip E032 */
    g_sim_bms.chg_volt_request_v = 501.5f;
    feed_chg_request(501.5f, 50.0f);
    drive_ms(1500U);
    ASSERT(!alarm_active(ALARM_BMS_VOLT_MISMATCH), "voltage difference <= 2V must not trip E032");

    /* Difference > 2.0V (e.g. 550.0V vs 500.0V -> 50V) */
    g_sim_bms.chg_volt_request_v = 550.0f;
    feed_chg_request(550.0f, 50.0f);
    drive_ms(500U); /* < 1000ms debounce */
    ASSERT(!alarm_active(ALARM_BMS_VOLT_MISMATCH), "debounce < 1000ms must not trip E032");

    drive_ms(600U); /* total 1100ms > 1000ms debounce */
    ASSERT(alarm_active(ALARM_BMS_VOLT_MISMATCH), "mismatch > 2V after 1000ms must trip E032");

    AlarmView_t av;
    Alarm_GetView(&av);
    ASSERT(av.worst_code == ALARM_BMS_VOLT_MISMATCH, "worst_code must be E032");
    ASSERT(av.highest_action == ALARM_ACT_STOP, "E032 action must be STOP");

    /* Recovery: voltage matches again (< ALARM_DB_COMM_CLEAR_MS 500ms) */
    g_sim_bms.chg_volt_request_v = 500.0f;
    feed_chg_request(500.0f, 50.0f);
    drive_ms(600U);
    ASSERT(!alarm_active(ALARM_BMS_VOLT_MISMATCH), "E032 must clear after recovery");

    printf("[PASS] test_bms_volt_mismatch_e032\n");
    return true;
}

/* ================================================================== *
 * Test: smart alarm time format (app_main.c logic)                   *
 *                                                                    *
 * Validates the same logic as the app_main.c alarm time block:       *
 *   - RTC invalid      → uptime fallback "HH:MM:SS"                  *
 *   - Event today      → "HH:MM:SS"                                  *
 *   - Event yesterday  → "HHhDD/MM"                                  *
 *   - Edge: 23:59 yesterday → "HHhDD/MM"                             *
 * ================================================================== */

/* Mirror of the app_main.c helper so we can call it in isolation.
 * Exposed here as a static helper that replicates the exact snprintf
 * format strings — keeps the test tightly coupled to the real logic. */
#include "bsp_rtc.h"
#include <stdio.h>
#include <string.h>

static void format_alarm_time_stub(char *out, size_t out_sz,
                                   uint32_t now_epoch_utc,
                                   uint32_t tick_now_ms,
                                   uint32_t tick_evt_ms,
                                   bool rtc_valid)
{
    /* Replicate exactly the logic in app_main.c */
    if (rtc_valid) {
        uint32_t delta_s = (tick_now_ms >= tick_evt_ms)
                           ? (tick_now_ms - tick_evt_ms) / 1000U
                           : 0U;
        uint32_t local_offset = (uint32_t)BSP_RTC_TIMEZONE_SEC;
        uint32_t evt_local = now_epoch_utc + local_offset - delta_s;
        uint32_t now_local = now_epoch_utc + local_offset;

        BSP_RTC_DateTime_t evt_dt, now_dt;
        BSP_RTC_EpochToDateTime(evt_local, &evt_dt);
        BSP_RTC_EpochToDateTime(now_local, &now_dt);

        bool same_day = (evt_dt.day == now_dt.day) &&
                        (evt_dt.month == now_dt.month);
        if (same_day) {
            snprintf(out, out_sz, "%02u:%02u:%02u",
                     (unsigned)evt_dt.hour,
                     (unsigned)evt_dt.minute,
                     (unsigned)evt_dt.second);
        } else {
            snprintf(out, out_sz, "%02uh%02u/%02u",
                     (unsigned)evt_dt.hour,
                     (unsigned)evt_dt.day,
                     (unsigned)evt_dt.month);
        }
    } else {
        uint32_t sec = tick_evt_ms / 1000U;
        uint32_t h = (sec / 3600U) % 24U;
        uint32_t m = (sec % 3600U) / 60U;
        uint32_t s = sec % 60U;
        snprintf(out, out_sz, "%02u:%02u:%02u",
                 (unsigned)h, (unsigned)m, (unsigned)s);
    }
}

static bool test_alarm_time_format_smart(void)
{
    printf("Running test_alarm_time_format_smart...\n");
    char buf[10];

    /* Reference moment: 2026-09-23 08:30:11 local (UTC+7)
     *                 = 2026-09-23 01:30:11 UTC                      */
    BSP_RTC_DateTime_t ref = {2026, 9, 23, 1, 30, 11, 0}; /* UTC */
    uint32_t ref_epoch_utc = BSP_RTC_DateTimeToEpoch(&ref);

    /* tick_now must be larger than the largest delta any case uses.
     * Case 3 uses 90,000,000 ms (25 h) → pick 100 h = 360,000,000 ms. */
    uint32_t tick_now = 360000000U; /* 100 h uptime */

    /* ----------------------------------------------------------------
     * Case 1: RTC invalid → uptime fallback
     * tick_evt = tick_now → uptime = 100h = "100:00:00"
     * But % 24 → "04:00:00" (100 % 24 = 4)
     * ---------------------------------------------------------------- */
    format_alarm_time_stub(buf, sizeof(buf),
                           ref_epoch_utc,
                           tick_now,
                           tick_now, /* event at same tick */
                           false);   /* RTC invalid */
    /* 100h → sec=360000, h=(360000/3600)%24 = 100%24 = 4 */
    ASSERT(strcmp(buf, "04:00:00") == 0,
           "RTC invalid: expected uptime 04:00:00 (100h mod 24)");

    /* ----------------------------------------------------------------
     * Case 2: Event occurred today (same 2026-09-23), 2 minutes ago
     * delta = 120 s
     * local evt = 2026-09-23 08:30:11 - 00:02:00 = 08:28:11
     * ---------------------------------------------------------------- */
    format_alarm_time_stub(buf, sizeof(buf),
                           ref_epoch_utc,
                           tick_now,
                           tick_now - 120000U, /* 2 min before */
                           true);
    ASSERT(strcmp(buf, "08:28:11") == 0,
           "Same-day event 2 min ago: expected 08:28:11");
    ASSERT(strlen(buf) == 8U, "Same-day format must be 8 chars");

    /* ----------------------------------------------------------------
     * Case 3: Event occurred yesterday (2026-09-22), 25 hours ago
     * delta = 25 * 3600 * 1000 = 90,000,000 ms  (tick_now=360M, no underflow)
     * local evt = 2026-09-23 08:30:11 - 25:00:00 = 2026-09-22 07:30:11
     * ---------------------------------------------------------------- */
    format_alarm_time_stub(buf, sizeof(buf),
                           ref_epoch_utc,
                           tick_now,
                           tick_now - 90000000U, /* 25h before */
                           true);
    ASSERT(strcmp(buf, "07h22/09") == 0,
           "Previous-day event 25h ago: expected 07h22/09");
    ASSERT(strlen(buf) == 8U, "Previous-day format must be 8 chars");

    /* ----------------------------------------------------------------
     * Case 4: Edge — event at 23:59:59 yesterday (2026-09-22)
     * local now = 2026-09-23 08:30:11
     * To get yesterday 23:59:59 local: delta = 8h30m12s = 30612 s
     * ---------------------------------------------------------------- */
    uint32_t delta_edge_s = (uint32_t)(8 * 3600 + 30 * 60 + 12); /* 30612s */
    format_alarm_time_stub(buf, sizeof(buf),
                           ref_epoch_utc,
                           tick_now,
                           tick_now - delta_edge_s * 1000U,
                           true);
    /* local evt = 2026-09-22 23:59:59 → "23h22/09" */
    ASSERT(buf[2] == 'h',
           "Edge 23:59 yesterday: must use previous-day format (HHhDD/MM)");
    ASSERT(strlen(buf) == 8U, "Edge format must be 8 chars");

    printf("[PASS] test_alarm_time_format_smart\n");
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
    ok &= test_bms_thermal_trip_limit_halts_on_4th();
    ok &= test_stage_thermal_trip_limit_halts_on_4th();
    ok &= test_controller_temperature_inhibit_suppresses_load_lost();
    ok &= test_dc_out_not_established();
    ok &= test_dc_out_summary_two_module_threshold();
    ok &= test_manual_stop_reason();
    ok &= test_e030_uses_fresh_multi_module_summary();
    ok &= test_bms_comm_lost_mid_charge();
    ok &= test_bms_no_pack_voltage();
    ok &= test_bms_low_pack_voltage_with_vmin();
    ok &= test_bms_low_pack_voltage_precharge_bypassed();
    ok &= test_config_temp_limit_halts_on_4th();
    ok &= test_bms_critical_alarm_mirrored();
    ok &= test_module_ac_undervolt_mirrored_and_derived();
    ok &= test_module_offline_uses_fresh_snapshot();
    ok &= test_w010_active_states_and_exact_recovery_debounce();
    ok &= test_acknowledge_clears_latched();
    ok &= test_start_with_no_module_or_bms_reports_fault_code();
    ok &= test_bms_alarm_timeout_auto_recovers_to_0000();
    ok &= test_bms_volt_mismatch_e032();
    ok &= test_alarm_time_format_smart();

    if (ok) { printf("\nALL TESTS PASSED.\n"); return 0; }
    printf("\nSOME TESTS FAILED.\n");
    return 1;
}
