/**
 * @file  alarm.h
 * @brief Unified station-level alarm/error subsystem.
 *
 * One code space over every fault source in the system:
 *   - BMS-reported   (mirror of BMS_ALARM_* bits from BMS_View_t)
 *   - module-reported (mirror of CHG_LIB_ALARM_* bits, aggregated over modules)
 *   - controller      (mirror of CHARGE_CTRL_FAULT_* -- report/log only)
 *   - DERIVED         faults nothing else detects: lost BMS link mid-charge,
 *                     pack disconnected, sudden DC load loss (hot unplug /
 *                     external contactor drop-out), DC circuit never closing,
 *                     AC phase loss, AC undervoltage.
 *
 * Pure policy (AGENTS.md sec 5-6): no BSP/HAL/main.h, the tick is passed in.
 * Runs once per 20 ms control tick from App_Loop, right after
 * ChargeController_Process() so the controller/module/BMS views are fresh.
 *
 * Each alarm has a debounce (set/clear) and an action:
 *   ALARM_ACT_INFO  -- surfaced + logged only
 *   ALARM_ACT_STOP  -- Alarm_Process() calls ChargeController_Stop()
 *   ALARM_ACT_ESTOP -- Alarm_Process() calls ChargeController_EmergencyStop()
 * The action is edge-triggered (issued once per activation), keeping a single
 * stop path through the charge controller.
 */
#ifndef APP_ALARM_ALARM_H
#define APP_ALARM_ALARM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============== Unified alarm code space ============== */
/* Order defines the bit position in AlarmView_t.active_mask. Keep <= 63. */
typedef enum {
    ALARM_NONE = 0,

    /* --- BMS-reported (1:1 with BMS_ALARM_* bits 0..12) --- */
    ALARM_BMS_LOW_PACK_VOLT,
    ALARM_BMS_LOW_CELL_VOLT,
    ALARM_BMS_HIGH_PACK_VOLT,
    ALARM_BMS_HIGH_CELL_VOLT,
    ALARM_BMS_TEMP_HIGH_CHG,
    ALARM_BMS_TEMP_HIGH_DCHG,
    ALARM_BMS_TEMP_LOW_CHG,
    ALARM_BMS_TEMP_LOW_DCHG,
    ALARM_BMS_TEMP_RELAY_HIGH,
    ALARM_BMS_OVER_CHG_CURR,
    ALARM_BMS_OVER_DCHG_CURR,
    ALARM_BMS_CELL_VOLT_DIFF,
    ALARM_BMS_LOW_SOC,

    /* --- module-reported (any active module raising the bit) --- */
    ALARM_MOD_HW_FAULT,
    ALARM_MOD_COMM_FAIL,
    ALARM_MOD_OVER_TEMP,
    ALARM_MOD_OVER_VOLT_OUT,
    ALARM_MOD_SHORT_CIRCUIT,
    ALARM_MOD_AC_UNDER_VOLT,
    ALARM_MOD_OVER_CURR_OUT,
    ALARM_MOD_PFC_FAULT,

    /* --- controller faults (mirror, report/log only) --- */
    ALARM_CTRL_NO_MODULE,
    ALARM_CTRL_MODULE_MISMATCH,
    ALARM_CTRL_INVALID_CONFIG,
    ALARM_CTRL_JACK_OVER_V,
    ALARM_CTRL_JACK_OVER_TEMP,
    ALARM_CTRL_EMERGENCY_STOP,

    /* --- derived / station-level (detected here, nowhere else) --- */
    ALARM_BMS_COMM_LOST,            /* RUNNING + BMS mode + BMS link gone      */
    ALARM_BMS_NO_PACK_VOLTAGE,      /* BMS online but pack voltage ~ 0         */
    ALARM_DC_LOAD_LOST,             /* current collapsed while commanded high  */
    ALARM_DC_OUT_NOT_ESTABLISHED,   /* relay closed, no current within window  */
    ALARM_AC_PHASE_LOSS,            /* one input phase lost                    */
    ALARM_AC_UNDERVOLT,             /* all input phases low                    */

    ALARM_CODE_COUNT
} AlarmCode_t;

typedef enum {
    ALARM_ACT_INFO = 0,
    ALARM_ACT_STOP,
    ALARM_ACT_ESTOP,
} AlarmAction_t;

/* ============== Public view ============== */

typedef struct {
    uint64_t      active_mask;    /* bit (1ULL << code) set while alarm active */
    uint64_t      latched_mask;   /* raw condition cleared, awaiting ack       */
    AlarmAction_t highest_action; /* worst action among active alarms          */
    AlarmCode_t   worst_code;     /* first active alarm at highest_action      */
    uint8_t       active_count;
} AlarmView_t;

/* ============== Event log ============== */

typedef struct {
    uint32_t uptime_ms;  /* tick at the edge                                  */
    uint16_t code;       /* AlarmCode_t                                       */
    uint8_t  action;     /* AlarmAction_t at the time of the edge             */
    uint8_t  event;      /* 1 = raised, 0 = cleared                           */
} AlarmLogEntry_t;

#define ALARM_LOG_DEPTH 24U

/* ============== API ============== */

void Alarm_Init(void);

/**
 * @brief Evaluate every alarm, run debounce, dispatch stop/e-stop.
 * @param now_tick BSP_GetTick() from the caller (App/Alarm is pure policy).
 */
void Alarm_Process(uint32_t now_tick);

void Alarm_GetView(AlarmView_t *view);

/**
 * @brief Operator acknowledged (HMI/physical RESET). Clears latched alarms
 *        whose underlying condition is no longer present. No-op otherwise.
 */
void Alarm_Acknowledge(uint32_t now_tick);

/**
 * @brief Copy up to @p max newest-first log entries into @p out.
 * @return number of entries written.
 */
uint8_t Alarm_GetLog(AlarmLogEntry_t *out, uint8_t max);

#ifdef __cplusplus
}
#endif

#endif /* APP_ALARM_ALARM_H */
