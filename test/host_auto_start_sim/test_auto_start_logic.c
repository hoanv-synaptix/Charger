/**
 * @file  test_auto_start_logic.c
 * @brief Independent logic unit tests (TDD style) for Auto Start (Workflow W4)
 *        and DWIN setting/navigation state machine.
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

#include "chg_lib.h"
#include "chg_lib_driver_tonhe.h"
#include "chg_lib_driver_maxwell.h"
#include "chg_lib_driver_lianming.h"
#include "chg_lib_can_backend.h"
#include "bms_core.h"
#include "charge_cycle_config.h"
#include "charge_controller.h"
#include "alarm.h"
#include "auto_start.h"

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

static void drive_step(uint32_t step_ms)
{
    mock_tick += step_ms;
    sim_bms_tick(mock_tick);
    sim_module_tick(g_sim_driver_kind, mock_tick);
    CHG_LIB_Process(mock_tick);
    BMS_Process(mock_tick);
    ChargeController_Process(mock_tick);
    Alarm_Process(mock_tick);
    AutoStart_Process(mock_tick);
}

static void drive_ms(uint32_t total_ms)
{
    for (uint32_t e = 0; e < total_ms; e += 20U) drive_step(20U);
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
    g_sim_bms.chg_volt_request_v = 400.0f; /* Matched to charger ceiling (400V) */
    g_sim_bms.chg_curr_request_a = 50.0f;
    g_sim_bms.bms_relay_allow = true;
    g_sim_bms.transmitting = true;
    g_sim_bms.low_pack_volt = 0;
    g_sim_bms.high_pack_volt = 0;
    g_sim_bms.over_chg_curr = 0;
}

static void setup_system(void)
{
    static bool registered = false;
    if (!registered) {
        CHG_LIB_RegisterDriver(CHG_LIB_DRV_MAXWELL, CHG_LIB_MaxwellDriverOps());
        CHG_LIB_RegisterDriver(CHG_LIB_DRV_LIANMING, CHG_LIB_LianmingDriverOps());
        CHG_LIB_RegisterDriver(CHG_LIB_DRV_TONHE, CHG_LIB_TonheDriverOps());
        registered = true;
    }
    mock_tick = 1000;
    g_sim_driver_kind = SIM_DRV_TONHE;
    sim_install_backend(g_sim_driver_kind);
    sim_module_reset(&g_sim_module, 1U, 0U);
    g_sim_modules[0].current_override = true;
    g_sim_modules[0].rated_current = 100.0f;
    g_sim_modules[0].voltage = 400.0f;
    g_sim_modules[0].voltage_override = true;

    sim_bms_reset(&g_sim_bms);
    healthy_bms(350.0f);

    BMS_Init();
    ChargeCycleConfig_Init();
    ChargeController_Init();
    Alarm_Init();
    AutoStart_Init();

    ChargeCycleConfig_t cfg;
    ChargeCycleConfig_GetDefaults(&cfg);
    cfg.module_type = CHARGE_MODULE_TYPE_TONHE;
    cfg.source_module_count = 1U;
    cfg.charge_source_mode = CHARGE_SOURCE_BMS_CONTROLLED;
    cfg.battery_capacity_ah = 100.0f;
    cfg.vmax_v = 400.0f;
    cfg.imax_c = 1.0f;
    cfg.auto_start_enabled = 1U;
    ChargeCycleConfig_Set(&cfg);
}

/* ========================================================================= */
/* Test Cases                                                                */
/* ========================================================================= */

/**
 * Case 1: Auto Start Disabled (Manual Mode).
 * Even if vehicle is plugged and healthy for > 10s, charger must stay IDLE.
 */
static bool test_auto_start_disabled_by_default_never_triggers(void)
{
    printf("Running test_auto_start_disabled_by_default_never_triggers...\n");
    setup_system();

    ChargeCycleConfig_t cfg;
    ChargeCycleConfig_Get(&cfg);
    cfg.auto_start_enabled = 0U; /* Manual mode */
    ChargeCycleConfig_Set(&cfg);

    drive_ms(5000U);

    const AutoStartState_t *st = AutoStart_GetState();
    ASSERT(st->plug_latched == true, "Plug should be latched after 5s");
    ChargeCtrlView_t cc;
    ChargeController_GetView(&cc);
    ASSERT(cc.state == CHARGE_CTRL_STATE_IDLE, "Must remain in IDLE in Manual mode");

    printf("[PASS] test_auto_start_disabled_by_default_never_triggers\n");
    return true;
}

/**
 * Case 2: Debounce 1500ms requirement.
 * Before 1500ms has elapsed, plug_latched must be false and charging must not start.
 * After 1500ms, plug_latched becomes true and charging automatically starts.
 */
static bool test_auto_start_debounce_1500ms_requirement(void)
{
    printf("Running test_auto_start_debounce_1500ms_requirement...\n");
    setup_system();

    /* Initially offline */
    g_sim_bms.transmitting = false;
    drive_ms(3500U); /* Ensure BMS is officially offline */
    AutoStart_ResetSession();

    /* Vehicle connects */
    g_sim_bms.transmitting = true;
    drive_ms(1000U); /* 1000ms elapsed since connection */

    const AutoStartState_t *st = AutoStart_GetState();
    ASSERT(st->plug_latched == false, "Must not latch before 1500ms debounce");
    ChargeCtrlView_t cc;
    ChargeController_GetView(&cc);
    ASSERT(cc.state == CHARGE_CTRL_STATE_IDLE, "Must stay IDLE before 1500ms debounce");

    /* Advance another 800ms (total 1800ms >= 1500ms) */
    drive_ms(800U);

    ASSERT(st->plug_latched == true, "Must be latched after 1500ms");
    ChargeController_GetView(&cc);
    ASSERT(cc.state != CHARGE_CTRL_STATE_IDLE, "Auto Start must have triggered charging");

    printf("[PASS] test_auto_start_debounce_1500ms_requirement\n");
    return true;
}

/**
 * Case 3: Anti Start-Stop Loop.
 * When user explicitly presses STOP, user_stopped is latched.
 * Auto Start must NEVER restart the charger while plugged in.
 */
static bool test_auto_start_user_stop_locks_out_auto_start(void)
{
    printf("Running test_auto_start_user_stop_locks_out_auto_start...\n");
    setup_system();

    /* Wait for auto start */
    drive_ms(2000U);
    ChargeCtrlView_t cc;
    ChargeController_GetView(&cc);
    ASSERT(cc.state != CHARGE_CTRL_STATE_IDLE, "Charger should be running");

    /* User presses STOP button */
    AutoStart_OnUserStop();
    ChargeController_Stop(mock_tick);

    /* Wait for controller to finish stopping and return to IDLE */
    drive_ms(1000U);
    ChargeController_GetView(&cc);
    ASSERT(cc.state == CHARGE_CTRL_STATE_IDLE, "Controller should be back in IDLE");

    const AutoStartState_t *st = AutoStart_GetState();
    ASSERT(st->user_stopped == true, "user_stopped must remain latched");

    /* Keep running for 10 seconds: charger must NEVER restart */
    drive_ms(10000U);
    ChargeController_GetView(&cc);
    ASSERT(cc.state == CHARGE_CTRL_STATE_IDLE, "Auto Start must not re-trigger after STOP");

    printf("[PASS] test_auto_start_user_stop_locks_out_auto_start\n");
    return true;
}

/**
 * Case 4: Manual START overrides user_stopped latch.
 * After user pressed STOP, if user presses START manually, charging resumes.
 */
static bool test_auto_start_manual_start_resumes_after_stop(void)
{
    printf("Running test_auto_start_manual_start_resumes_after_stop...\n");
    setup_system();

    /* Start and then user stops */
    drive_ms(2000U);
    AutoStart_OnUserStop();
    ChargeController_Stop(mock_tick);
    drive_ms(1000U);

    const AutoStartState_t *st = AutoStart_GetState();
    ASSERT(st->user_stopped == true, "Latched stopped");

    /* User presses START manually */
    AutoStart_OnUserStart();
    ChargeController_Start(CHARGE_CTRL_OWNER_DWIN, false, mock_tick);

    ASSERT(st->user_stopped == false, "user_stopped cleared");
    ASSERT(st->charge_completed == false, "charge_completed cleared");

    drive_ms(200U);
    ChargeCtrlView_t cc;
    ChargeController_GetView(&cc);
    ASSERT(cc.state != CHARGE_CTRL_STATE_IDLE, "Charging resumed successfully");

    printf("[PASS] test_auto_start_manual_start_resumes_after_stop\n");
    return true;
}

/**
 * Case 5: Battery already full on plug-in (Cell Voltage reached).
 * Cold boot / AC recovery with already-full battery:
 * Auto Start must latch charge_completed = true and NOT start charging.
 */
static bool test_auto_start_battery_full_cell_voltage_triggers_complete(void)
{
    printf("Running test_auto_start_battery_full_cell_voltage_triggers_complete...\n");
    setup_system();

    ChargeCycleConfig_t cfg;
    ChargeCycleConfig_Get(&cfg);
    cfg.cell_volt_enabled = 1U;
    cfg.cell_volt_5_v = 3.65f; /* 3.65V threshold */
    ChargeCycleConfig_Set(&cfg);

    /* Battery cell is at 3.66V */
    g_sim_bms.max_cell_mv = 3660;

    drive_ms(2000U);

    ASSERT(AutoStart_IsChargeCompleted() == true, "Must latch charge_completed");
    ChargeCtrlView_t cc;
    ChargeController_GetView(&cc);
    ASSERT(cc.state == CHARGE_CTRL_STATE_IDLE, "Must stay IDLE, no overcharge!");

    printf("[PASS] test_auto_start_battery_full_cell_voltage_triggers_complete\n");
    return true;
}

/**
 * Case 6: Battery already full on plug-in (SOC reached).
 */
static bool test_auto_start_battery_full_soc_triggers_complete(void)
{
    printf("Running test_auto_start_battery_full_soc_triggers_complete...\n");
    setup_system();

    ChargeCycleConfig_t cfg;
    ChargeCycleConfig_Get(&cfg);
    cfg.cell_volt_enabled = 0U; /* Pure SOC mode */
    cfg.soc_enabled = 1U;
    cfg.soc_5_pct = 99.0f; /* 99% threshold */
    ChargeCycleConfig_Set(&cfg);

    /* Battery SOC is 100% */
    g_sim_bms.soc_pct = 100;

    drive_ms(2000U);

    ASSERT(AutoStart_IsChargeCompleted() == true, "Must latch charge_completed on SOC");
    ChargeCtrlView_t cc;
    ChargeController_GetView(&cc);
    ASSERT(cc.state == CHARGE_CTRL_STATE_IDLE, "Must stay IDLE");

    printf("[PASS] test_auto_start_battery_full_soc_triggers_complete\n");
    return true;
}

/**
 * Case 6b: Dual mode (Cell Voltage & SOC enabled).
 * BMS reports SOC 100% early, but cell max voltage is only 3.20V (< 3.65V).
 * AutoStart must NOT consider the battery full, and must start charging normally!
 */
static bool test_auto_start_dual_mode_soc100_does_not_block_undercharged_cell(void)
{
    printf("Running test_auto_start_dual_mode_soc100_does_not_block_undercharged_cell...\n");
    setup_system();

    ChargeCycleConfig_t cfg;
    ChargeCycleConfig_Get(&cfg);
    cfg.auto_start_enabled = 1U;
    cfg.cell_volt_enabled = 1U;
    cfg.cell_volt_5_v = 3.65f;
    cfg.soc_enabled = 1U;
    cfg.soc_5_pct = 99.0f;
    ChargeCycleConfig_Set(&cfg);

    /* BMS says SOC 100%, but cells are only at 3.20V */
    g_sim_bms.soc_pct = 100;
    g_sim_bms.max_cell_mv = 3200;

    drive_ms(2000U);

    ASSERT(AutoStart_IsChargeCompleted() == false, "Must NOT latch COMPLETE when cell voltage < 3.65V");
    ChargeCtrlView_t cc;
    ChargeController_GetView(&cc);
    ASSERT(cc.state == CHARGE_CTRL_STATE_RUNNING, "Must proceed to RUNNING state");

    printf("[PASS] test_auto_start_dual_mode_soc100_does_not_block_undercharged_cell\n");
    return true;
}

/**
 * Case 7: Battery already full on plug-in (Pack Voltage reached).
 */
static bool test_auto_start_battery_full_pack_volt_triggers_complete(void)
{
    printf("Running test_auto_start_battery_full_pack_volt_triggers_complete...\n");
    setup_system();

    ChargeCycleConfig_t cfg;
    ChargeCycleConfig_Get(&cfg);
    cfg.vmax_v = 400.0f;
    ChargeCycleConfig_Set(&cfg);

    /* Battery pack voltage is 402V */
    g_sim_bms.pack_voltage_v = 402.0f;

    drive_ms(2000U);

    ASSERT(AutoStart_IsChargeCompleted() == true, "Must latch charge_completed on pack volt");
    ChargeCtrlView_t cc;
    ChargeController_GetView(&cc);
    ASSERT(cc.state == CHARGE_CTRL_STATE_IDLE, "Must stay IDLE");

    printf("[PASS] test_auto_start_battery_full_pack_volt_triggers_complete\n");
    return true;
}

/**
 * Case 8: User Reset after COMPLETE does not cause immediate restart loop.
 * User pressing RESET acknowledges COMPLETE and returns to READY,
 * but Auto Start must not immediately re-charge the full battery.
 */
static bool test_auto_start_user_reset_complete_does_not_restart_full_battery(void)
{
    printf("Running test_auto_start_user_reset_complete_does_not_restart_full_battery...\n");
    setup_system();

    ChargeCycleConfig_t cfg;
    ChargeCycleConfig_Get(&cfg);
    cfg.cell_volt_enabled = 0U; /* Pure SOC mode */
    cfg.soc_enabled = 1U;
    cfg.soc_5_pct = 99.0f;
    ChargeCycleConfig_Set(&cfg);

    g_sim_bms.soc_pct = 100;
    drive_ms(2000U);
    ASSERT(AutoStart_IsChargeCompleted() == true, "Completed");

    /* User presses RESET */
    ChargeController_AcknowledgeCompletion();
    AutoStart_OnUserReset();

    const AutoStartState_t *st = AutoStart_GetState();
    ASSERT(st->user_stopped == true, "RESET sets user_stopped to prevent auto-start loop");

    drive_ms(3000U);

    ChargeCtrlView_t cc;
    ChargeController_GetView(&cc);
    ASSERT(cc.state == CHARGE_CTRL_STATE_IDLE, "Must stay IDLE after RESET");

    printf("[PASS] test_auto_start_user_reset_complete_does_not_restart_full_battery\n");
    return true;
}

/**
 * Case 9: Cable Unplug (> 5s CAN loss) resets session cleanly.
 * When plugged into another vehicle (or re-plugged), a fresh session starts.
 */
static bool test_auto_start_cable_unplug_5s_resets_session(void)
{
    printf("Running test_auto_start_cable_unplug_5s_resets_session...\n");
    setup_system();

    /* User stopped previous session */
    drive_ms(2000U);
    AutoStart_OnUserStop();
    ChargeController_Stop(mock_tick);
    drive_ms(1000U);

    const AutoStartState_t *st = AutoStart_GetState();
    ASSERT(st->user_stopped == true, "Latched stopped");

    /* Operator unplugs cable: CAN stops transmitting */
    g_sim_bms.transmitting = false;

    /* Before 5000ms (at 3000ms): BMS is still within offline timeout */
    drive_ms(3000U);
    BMS_View_t bv;
    BMS_GetView(&bv);
    ASSERT(bv.online == true, "BMS still debouncing offline at 3s");
    ASSERT(st->user_stopped == true, "Session still intact at 3s");

    /* At 5200ms (> 5000ms BMS_OFFLINE_TIMEOUT_MS): BMS becomes offline */
    drive_ms(2200U);
    BMS_GetView(&bv);
    ASSERT(bv.online == false, "BMS officially offline after 5s");
    ASSERT(st->plug_latched == false, "plug_latched unlatched when offline");

    /* Another 1200ms (> 1000ms AUTO_START_CAN_OFFLINE_RESET_MS): session reset! */
    drive_ms(1200U);
    ASSERT(st->user_stopped == false, "Session reset: user_stopped cleared");
    ASSERT(st->charge_completed == false, "Session reset: charge_completed cleared");
    ASSERT(st->plug_latched == false, "Session reset: plug_latched cleared");

    /* Plug into a new vehicle with 40% SOC */
    g_sim_bms.transmitting = true;
    g_sim_bms.soc_pct = 40;
    g_sim_bms.max_cell_mv = 3150;

    drive_ms(2000U);

    ChargeCtrlView_t cc;
    ChargeController_GetView(&cc);
    ASSERT(cc.state != CHARGE_CTRL_STATE_IDLE, "New vehicle automatically starts charging!");

    printf("[PASS] test_auto_start_cable_unplug_5s_resets_session\n");
    return true;
}

/**
 * Case 10: System Alarm Interlock.
 * When an active alarm requires STOP/ESTOP, Auto Start must be strictly blocked.
 */
static bool test_auto_start_blocked_by_system_alarm(void)
{
    printf("Running test_auto_start_blocked_by_system_alarm...\n");
    setup_system();

    /* Trip high pack voltage alarm (requires STOP) */
    g_sim_bms.high_pack_volt = 2U;

    drive_ms(600U);

    AlarmView_t av;
    Alarm_GetView(&av);
    ASSERT(av.highest_action >= ALARM_ACT_STOP, "Alarm active");

    drive_ms(2000U);

    ChargeCtrlView_t cc;
    ChargeController_GetView(&cc);
    ASSERT(cc.state == CHARGE_CTRL_STATE_IDLE,
           "Must never auto start with active system alarm");

    printf("[PASS] test_auto_start_blocked_by_system_alarm\n");
    return true;
}

/**
 * Case 11: BMS Hard Alarm Interlock.
 * When BMS reports critical alarms (over current, cell overvoltage, etc.), Auto Start blocked.
 */
static bool test_auto_start_blocked_by_bms_hard_alarm(void)
{
    printf("Running test_auto_start_blocked_by_bms_hard_alarm...\n");
    setup_system();

    /* BMS has over-charge current alarm (severity 2) */
    g_sim_bms.over_chg_curr = 2U;

    drive_ms(2000U);

    ChargeCtrlView_t cc;
    ChargeController_GetView(&cc);
    ASSERT(cc.state == CHARGE_CTRL_STATE_IDLE, "Must not start with BMS hard alarm");

    printf("[PASS] test_auto_start_blocked_by_bms_hard_alarm\n");
    return true;
}

/**
 * Case 12: Fault Lockout on E004 High Cell Voltage Prevents Chattering Oscillation.
 * When BMS trips E004 (high_cell_volt) during charge:
 * 1. Charger stops immediately (STOPPING -> IDLE, stop_reason = PROTECTION).
 * 2. AutoStart latches fault_latched = true.
 * 3. BMS cell voltage drops and clears error flag (high_cell_volt = 0).
 * 4. AutoStart must NOT auto-start charging again! (Locks out infinite loop).
 */
static bool test_auto_start_fault_lockout_on_e004_prevents_chattering(void)
{
    printf("Running test_auto_start_fault_lockout_on_e004_prevents_chattering...\n");
    setup_system();

    ChargeCycleConfig_t cfg;
    ChargeCycleConfig_Get(&cfg);
    cfg.auto_start_enabled = 1U;
    cfg.cell_volt_enabled = 1U;
    cfg.cell_volt_5_v = 3.60f; /* Higher than BMS trip point */
    ChargeCycleConfig_Set(&cfg);

    /* 1. Let auto start engage */
    drive_ms(2000U);

    ChargeCtrlView_t cc;
    ChargeController_GetView(&cc);
    ASSERT(cc.state == CHARGE_CTRL_STATE_RUNNING, "Must be charging after 2s");

    /* 2. Battery reaches 100% and cell spikes -> BMS fires E004 (high_cell_volt = 1) */
    g_sim_bms.high_cell_volt = 1U;
    drive_ms(200U);

    ChargeController_GetView(&cc);
    ASSERT(cc.state == CHARGE_CTRL_STATE_FAULT || cc.state == CHARGE_CTRL_STATE_STOPPING,
           "Must transition to FAULT/STOPPING immediately on E004 high cell voltage");
    const AutoStartState_t *state = AutoStart_GetState();
    ASSERT(state->fault_latched == true, "fault_latched must be TRUE after protection stop");

    /* 3. Cell relaxes: BMS clears high_cell_volt back to 0 */
    g_sim_bms.high_cell_volt = 0U;
    /* Run for 5 seconds with healthy BMS and plugged cable */
    drive_ms(5000U);

    /* CRITICAL ASSERTION: Charger must NOT auto-start while fault_latched is true! */
    ChargeController_GetView(&cc);
    ASSERT(cc.state != CHARGE_CTRL_STATE_RUNNING,
           "CRITICAL: Must NOT auto-start after runtime fault (prevents chattering loop)");
    ASSERT(state->fault_latched == true, "fault_latched must remain latched while plugged");

    /* 4. Unplug cable for > 6s (> 5000ms BMS_OFFLINE_TIMEOUT_MS + 1000ms AUTO_START_CAN_OFFLINE_RESET_MS) */
    g_sim_bms.transmitting = false;
    drive_ms(6500U);

    ASSERT(state->fault_latched == false, "Unplugging cable must reset fault_latched");

    printf("[PASS] test_auto_start_fault_lockout_on_e004_prevents_chattering\n");
    return true;
}

/**
 * Case 13: Manual User Start Can Clear Fault Lockout.
 * If user explicitly presses START after a fault, fault_latched is cleared and charge resumes.
 */
static bool test_auto_start_manual_start_can_clear_fault_lockout(void)
{
    printf("Running test_auto_start_manual_start_can_clear_fault_lockout...\n");
    setup_system();

    ChargeCycleConfig_t cfg;
    ChargeCycleConfig_Get(&cfg);
    cfg.auto_start_enabled = 1U;
    ChargeCycleConfig_Set(&cfg);

    /* Start and trip fault */
    drive_ms(2000U);
    g_sim_bms.high_cell_volt = 1U;
    drive_ms(500U);

    const AutoStartState_t *state = AutoStart_GetState();
    ASSERT(state->fault_latched == true, "Must be fault latched");

    /* Clear fault on BMS side */
    g_sim_bms.high_cell_volt = 0U;
    drive_ms(500U);

    /* On DWIN HMI, when in ERROR state, user presses Reset button to clear fault back to IDLE */
    (void)ChargeController_ResetEmergencyStop(mock_tick);
    (void)ChargeController_ResetFaultIfSafe(mock_tick);
    Alarm_Acknowledge(mock_tick);
    AutoStart_OnUserReset();

    /* User clicks START manually */
    AutoStart_OnUserStart();
    ASSERT(state->fault_latched == false, "Manual START must clear fault_latched");

    (void)ChargeController_Start(CHARGE_CTRL_OWNER_DWIN, false, mock_tick);
    drive_ms(200U);

    ChargeCtrlView_t cc;
    ChargeController_GetView(&cc);
    ASSERT(cc.state == CHARGE_CTRL_STATE_RUNNING || cc.state == CHARGE_CTRL_STATE_READY,
           "Charge must resume after user manual START");

    printf("[PASS] test_auto_start_manual_start_can_clear_fault_lockout\n");
    return true;
}

/**
 * Two-Way Handshake: When cable is plugged in before AC breaker is turned on,
 * AutoStart must stay in safe IDLE standby and NOT trigger E027 or lock out.
 * When AC power is established, AutoStart must immediately start charging.
 */
static bool test_auto_start_waits_for_module_power_before_starting(void)
{
    printf("Running test_auto_start_waits_for_module_power_before_starting...\n");
    setup_system();

    ChargeCycleConfig_t cfg;
    ChargeCycleConfig_Get(&cfg);
    cfg.auto_start_enabled = 1U;
    ChargeCycleConfig_Set(&cfg);

    /* Module is unpowered / silent (AC breaker is off) */
    g_sim_modules[0].silent = true;

    /* BMS is plugged in and transmitting for 5 seconds */
    drive_ms(5000U);

    ChargeCtrlView_t cc;
    ChargeController_GetView(&cc);
    const AutoStartState_t *state = AutoStart_GetState();

    /* Two-Way Handshake requirement: Must stay in IDLE, no fault latched, within 30s grace period */
    ASSERT(cc.state == CHARGE_CTRL_STATE_IDLE, "Must stay in IDLE waiting for module power");
    ASSERT(state->fault_latched == false, "Must not latch fault while waiting for AC power");
    ASSERT(AutoStart_IsModuleTimeout() == false, "Must not be timed out at 5s");

    AlarmView_t av;
    Alarm_GetView(&av);
    ASSERT((av.active_mask & (1ULL << ALARM_CTRL_NO_MODULE)) == 0, "E027 must NOT be reported during grace period");

    /* Now user turns on AC breaker at t=5s -> module boots and communicates */
    g_sim_modules[0].silent = false;
    drive_ms(300U);

    ChargeController_GetView(&cc);
    ASSERT(cc.state == CHARGE_CTRL_STATE_RUNNING || cc.state == CHARGE_CTRL_STATE_READY,
           "Must automatically start charging once module power is restored");

    printf("[PASS] test_auto_start_waits_for_module_power_before_starting\n");
    return true;
}

/**
 * Module Timeout: When vehicle is plugged in but AC power is NOT turned on for >30s,
 * E027 warning is displayed. When AC power is eventually turned on, E027 self-clears
 * and charging automatically begins without requiring cable replug or reset button.
 */
static bool test_auto_start_module_timeout_reports_e027_and_recovers_on_ac_power(void)
{
    printf("Running test_auto_start_module_timeout_reports_e027_and_recovers_on_ac_power...\n");
    setup_system();

    ChargeCycleConfig_t cfg;
    ChargeCycleConfig_Get(&cfg);
    cfg.auto_start_enabled = 1U;
    ChargeCycleConfig_Set(&cfg);

    /* Module is silent (AC breaker off) */
    g_sim_modules[0].silent = true;

    /* Wait past 30 seconds timeout */
    drive_ms(31000U);

    ASSERT(AutoStart_IsModuleTimeout() == true, "Must flag module timeout past 30s");

    AlarmView_t av;
    Alarm_GetView(&av);
    ASSERT((av.active_mask & (1ULL << ALARM_CTRL_NO_MODULE)) != 0, "E027 must be active after 30s timeout");

    ChargeCtrlView_t cc;
    ChargeController_GetView(&cc);
    const AutoStartState_t *state = AutoStart_GetState();
    ASSERT(cc.state == CHARGE_CTRL_STATE_IDLE, "Controller must still be in safe IDLE standby");
    ASSERT(state->fault_latched == false, "Must not latch fatal fault (allow auto-recovery)");

    /* Now user turns on AC breaker -> module comes online */
    g_sim_modules[0].silent = false;
    drive_ms(300U);

    ASSERT(AutoStart_IsModuleTimeout() == false, "Timeout warning must clear when module online");
    Alarm_GetView(&av);
    ASSERT((av.active_mask & (1ULL << ALARM_CTRL_NO_MODULE)) == 0, "E027 must clear when module online");

    ChargeController_GetView(&cc);
    ASSERT(cc.state == CHARGE_CTRL_STATE_RUNNING || cc.state == CHARGE_CTRL_STATE_READY,
           "Must seamlessly auto start charging without requiring cable replug or reset button");

    printf("[PASS] test_auto_start_module_timeout_reports_e027_and_recovers_on_ac_power\n");
    return true;
}

/* ========================================================================= */
/* Test Runner                                                               */
/* ========================================================================= */

int main(void)
{
    printf("=== RUNNING AUTO START WORKFLOW W4 LOGIC UNIT TESTS ===\n");
    bool ok = true;

    ok &= test_auto_start_disabled_by_default_never_triggers();
    ok &= test_auto_start_debounce_1500ms_requirement();
    ok &= test_auto_start_user_stop_locks_out_auto_start();
    ok &= test_auto_start_manual_start_resumes_after_stop();
    ok &= test_auto_start_battery_full_cell_voltage_triggers_complete();
    ok &= test_auto_start_battery_full_soc_triggers_complete();
    ok &= test_auto_start_dual_mode_soc100_does_not_block_undercharged_cell();
    ok &= test_auto_start_battery_full_pack_volt_triggers_complete();
    ok &= test_auto_start_user_reset_complete_does_not_restart_full_battery();
    ok &= test_auto_start_cable_unplug_5s_resets_session();
    ok &= test_auto_start_blocked_by_system_alarm();
    ok &= test_auto_start_blocked_by_bms_hard_alarm();
    ok &= test_auto_start_fault_lockout_on_e004_prevents_chattering();
    ok &= test_auto_start_manual_start_can_clear_fault_lockout();
    ok &= test_auto_start_waits_for_module_power_before_starting();
    ok &= test_auto_start_module_timeout_reports_e027_and_recovers_on_ac_power();

    if (ok) {
        printf(">>> ALL 16 AUTO START LOGIC UNIT TESTS PASSED 100%%! <<<\n");
        return 0;
    } else {
        printf(">>> AUTO START LOGIC UNIT TESTS FAILED! <<<\n");
        return 1;
    }
}
