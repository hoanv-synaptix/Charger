/**
 * @file charge_controller.h
 * @brief Charge cycle controller - state machine for automated charging
 */

#ifndef CHARGE_CONTROLLER_H
#define CHARGE_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============== Controller State ============== */

typedef enum {
    CHARGE_CTRL_STATE_IDLE = 0,
    CHARGE_CTRL_STATE_READY,
    CHARGE_CTRL_STATE_RUNNING,
    CHARGE_CTRL_STATE_STOPPING,
    CHARGE_CTRL_STATE_FAULT,
    CHARGE_CTRL_STATE_PRECHARGE,
} ChargeCtrlState_t;

typedef enum {
    CHARGE_LIMIT_SOURCE_NONE = 0,
    CHARGE_LIMIT_SOURCE_CELL_VOLTAGE,
    CHARGE_LIMIT_SOURCE_TEMPERATURE,
    CHARGE_LIMIT_SOURCE_SOC,
} ChargeLimitSource_t;

typedef enum {
    CHARGE_STAGE_BAND_NONE = 0,
    CHARGE_STAGE_BAND_BELOW_MIN,
    CHARGE_STAGE_BAND_1_2,
    CHARGE_STAGE_BAND_2_3,
    CHARGE_STAGE_BAND_3_4,
    CHARGE_STAGE_BAND_4_5,
    CHARGE_STAGE_BAND_ABOVE_MAX,
} ChargeStageBand_t;

/* ============== Owner (who started the cycle) ============== */

typedef enum {
    CHARGE_CTRL_OWNER_NONE = 0,
    CHARGE_CTRL_OWNER_PC,
    CHARGE_CTRL_OWNER_DWIN,
} ChargeCtrlOwner_t;

typedef enum {
    CHARGE_STOP_NONE = 0,
    CHARGE_STOP_USER_COMMAND,
    CHARGE_STOP_STAGE_INHIBIT,
    CHARGE_STOP_BMS_OFFLINE,
    CHARGE_STOP_BMS_ALARM,
    CHARGE_STOP_PROTECTION,
    CHARGE_STOP_MODULE_TIMEOUT,
    CHARGE_STOP_MODULE_FAULT,
    CHARGE_STOP_MODULE_MISMATCH,
    CHARGE_STOP_EMERGENCY,
    CHARGE_STOP_PRECONDITION,
    CHARGE_STOP_VOLTAGE_REACHED,
    CHARGE_STOP_CELL_VOLTAGE_REACHED,
    CHARGE_STOP_SOC_REACHED,
    CHARGE_STOP_PRECHARGE_COMPLETE,
} ChargeStopReason_t;

#define PRECHARGE_VOLTAGE_TOLERANCE_V  1.0f
#define PRECHARGE_HOLD_MS              60000U
/* Intentionally zero: pre-charge waits for an exhausted BMS to wake until
 * the operator stops it or another protection path stops it. */
#define PRECHARGE_BMS_WAKE_TIMEOUT_MS  0U

/* ============== Fault Flags ============== */

#define CHARGE_CTRL_FAULT_NONE                     0U
#define CHARGE_CTRL_FAULT_NO_DRIVER              (1U << 0)
#define CHARGE_CTRL_FAULT_NO_MODULE               (1U << 1)
#define CHARGE_CTRL_FAULT_MODULE_COUNT_MISMATCH   (1U << 2)
#define CHARGE_CTRL_FAULT_BMS_OFFLINE            (1U << 3)
#define CHARGE_CTRL_FAULT_BMS_STALE              (1U << 4)
#define CHARGE_CTRL_FAULT_BMS_ALARM              (1U << 5)
#define CHARGE_CTRL_FAULT_INVALID_CONFIG         (1U << 6)
#define CHARGE_CTRL_FAULT_RESERVED_7             (1U << 7)
#define CHARGE_CTRL_FAULT_PROTECT_JACK_V         (1U << 8)
#define CHARGE_CTRL_FAULT_PROTECT_JACK_TEMP      (1U << 9)
#define CHARGE_CTRL_FAULT_RESERVED_10            (1U << 10)
#define CHARGE_CTRL_FAULT_EMERGENCY_STOP         (1U << 11)

/* ============== Controller View ============== */

typedef struct {
    ChargeCtrlState_t state;
    uint8_t running;
    uint8_t faulted;
    uint8_t derating;
    uint8_t inhibit;
    uint8_t start_requested;
    uint8_t emergency_stop;
    float target_voltage_v;
    float target_current_total_a;
    float target_current_per_module_a;
    float applied_voltage_v;
    float applied_current_per_module_a;
    uint32_t fault_flags;
    uint32_t last_update_tick;
    ChargeCtrlOwner_t owner;
    uint8_t source_module_count;
    uint8_t actual_module_count;
    uint8_t charge_source_mode;
    uint8_t active_limit_source;
    uint8_t active_stage_band;
    float active_limit_current_c;
    ChargeStopReason_t stop_reason;
    /** True when the battery relay should be closed. This is a latch, not
     * a continuous voltage gate: it closes once state is RUNNING and the
     * worst-case (minimum) online module output voltage reaches
     * CHARGE_CTRL_RELAY_ARM_VOLT_PCT of the reference (pack) voltage, then STAYS closed
     * through normal voltage/current fluctuation — voltage dropping back
     * below that threshold does NOT reopen it. Only leaving RUNNING, or —
     * when charge_source_mode is BMS-Controlled — the BMS itself reporting
     * unsafe (BMS_ShouldCloseChargeRelay() going false) reopens it; both
     * are checked every tick even while latched closed. Standalone (no-BMS)
     * mode never requires a BMS at all. Re-computed every
     * ChargeController_Process() call — the composition root
     * (App/System/app_main.c) just mirrors this onto the relay GPIO, it
     * does not decide the condition itself. */
    uint8_t relay_should_close;
} ChargeCtrlView_t;

/* ============== Public API ============== */

/**
 * @brief Initialize charge controller
 */
void ChargeController_Init(void);

/**
 * @brief Process charge controller (call every 20ms)
 * @param now_tick Current HAL_GetTick() value
 */
void ChargeController_Process(uint32_t now_tick);

/**
 * @brief Check if preconditions for starting are met (does not start)
 * @param fault_flags_out Optional pointer to receive fault flags if check fails
 * @return true if preconditions pass, false if cannot start
 */
bool ChargeController_CheckPreconditions(uint32_t *fault_flags_out);

/**
 * @brief Request to start charge cycle
 * @param owner Who initiated the start (PC or DWIN)
 * @param now_tick Current tick (BSP_GetTick() from the caller -- App/Charge
 *        is pure policy and must not read the tick itself, AGENTS.md sec 5-6)
 * @return true if start request accepted, false if cannot start
 */
bool ChargeController_Start(ChargeCtrlOwner_t owner, bool manual_mode, uint32_t now_tick);

/** Start the BMS wake/recovery pre-charge flow using config Vlow/Ilow. */
bool ChargeController_StartPrecharge(ChargeCtrlOwner_t owner, uint32_t now_tick);

/** Request the normal controlled stop path for an active pre-charge. */
void ChargeController_StopPrecharge(uint32_t now_tick);
/** Clear the current charge/pre-charge fault only after the controller
 * verifies the originating mode's conditions and output path are safe. */
bool ChargeController_ResetFaultIfSafe(uint32_t now_tick);
void ChargeController_SetManualTarget(float voltage, float current);
bool ChargeController_IsManualMode(void);

/**
 * @brief Feed the jack/connector temperature reading (max of the 4 NTC
 *        channels) for the soft-derating check. Call once per control
 *        cycle from the composition root, before ChargeController_Process().
 *        App/Charge is pure policy and must not read BSP_ADC itself
 *        (AGENTS.md sec 5-6). NaN/Inf are ignored (previous value kept).
 */
void ChargeController_SetJackTempC(float temp_c);

/**
 * @brief Stop charge cycle (controlled stop)
 * @param now_tick Current tick (BSP_GetTick() from the caller)
 */
void ChargeController_Stop(uint32_t now_tick);

/**
 * @brief Emergency stop - immediately halt charging
 * @param now_tick Current tick (BSP_GetTick() from the caller)
 */
void ChargeController_EmergencyStop(uint32_t now_tick);

/**
 * @brief Check if charge cycle is currently running
 * @return true if running, false otherwise
 */
bool ChargeController_IsRunning(void);

/**
 * @brief Operator dismissed the "charge complete" indication (HMI RESET
 *        button while the screen shows COMPLETE). Clears the completion
 *        marker (stop_reason) so the HMI/PC stop reporting COMPLETE and the
 *        button falls back to START -- WITHOUT starting a new cycle.
 * @note  Only acts when state is IDLE and stop_reason is one of the
 *        "target reached" values; a no-op otherwise. Does not touch the
 *        relay, fault flags, state or owner.
 */
void ChargeController_AcknowledgeCompletion(void);

/**
 * @brief Get controller status view
 * @param view Pointer to view structure to fill
 */
void ChargeController_GetView(ChargeCtrlView_t *view);

#ifdef __cplusplus
}
#endif

#endif /* CHARGE_CONTROLLER_H */
