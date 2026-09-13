/**
 * @file chg_lib.h
 * @brief Charger Core - Abstract Interface and Data Types
 * @note This module provides the abstract interface for all charger drivers.
 *
 * Architecture:
 *   ┌─────────────────────────────────────────┐
 *   │           app_charger.c                 │
 *   │      (Upper Application Layer)          │
 *   └─────────────────┬───────────────────────┘
 *                     │ uses
 *                     ↓
 *   ┌─────────────────────────────────────────┐
 *   │            charger_core.c/h               │
 *   │      (Abstract Interface Layer)           │
 *   │                                         │
 *   │  - CHG_LIB_DriverOps_t (interface)         │
 *   │  - CHG_LIB_ModuleView_t (data model)       │
 *   │  - Driver Registry (Select driver)     │
 *   │  - API Wrappers (CHG_LIB_SetVoltage...)    │
 *   └─────────────────┬───────────────────────┘
 *                     │ implements
 *     ┌───────────────┼───────────────┐
 *     ↓               ↓               ↓
 * ┌─────────┐   ┌─────────┐   ┌─────────┐
 * │ Maxwel  │   │ Lianming│   │ TonHe   │
 * └─────────┘   └─────────┘   └─────────┘
 *
 * Usage:
 *   1. Register drivers: CHG_LIB_RegisterDriver(DRIVER_ID, DRIVER_OPS);
 *   2. Select active driver: CHG_LIB_SelectDriver(DRIVER_ID);
 *   3. Initialize: CHG_LIB_Init();
 *   4. Add module(s): CHG_LIB_AddModule(addr, group);
 *   5. Control: CHG_LIB_SetVoltage(), CHG_LIB_Start(), etc.
 *   6. Process: Call CHG_LIB_Process() periodically
 *   7. Feed CAN frames: CHG_LIB_FeedCanFrame() from the main loop
 *
 * Design Patterns:
 *   - Abstract Factory: Creates driver instances via ops table
 *   - Strategy Pattern: Swappable driver implementations
 *   - Proxy:charger_core delegates to active driver
 */

#ifndef CHG_LIB_CORE_H
#define CHG_LIB_CORE_H

#include <stdbool.h>
#include <stdint.h>
#include "chg_lib_can_backend.h"

#define CHG_LIB_MAX_DRV   8

typedef enum {
    CHG_LIB_DRV_NONE     = 0,
    CHG_LIB_DRV_MAXWELL  = 1,
    CHG_LIB_DRV_LIANMING = 2,
    CHG_LIB_DRV_TONHE    = 3,
} CHG_LIB_DriverId_t;

typedef enum {
    CHG_LIB_STATE_IDLE = 0,
    CHG_LIB_STATE_STARTING,
    CHG_LIB_STATE_RUNNING,
    CHG_LIB_STATE_WARNING,
    CHG_LIB_STATE_OFFLINE,
    CHG_LIB_STATE_FAULT,
    CHG_LIB_STATE_RECOVERING,
    CHG_LIB_STATE_STOPPING,
} CHG_LIB_State_t;

typedef enum {
    CHG_LIB_ALARM_NONE              = 0x0000,
    CHG_LIB_ALARM_HW_FAULT          = (1U << 0),  /* Lỗi phần cứng chung */
    CHG_LIB_ALARM_COMM_FAIL         = (1U << 1),  /* Mất kết nối giao tiếp */
    CHG_LIB_ALARM_OVER_TEMP         = (1U << 2),  /* Quá nhiệt */
    CHG_LIB_ALARM_OVER_VOLTAGE_OUT  = (1U << 3),  /* Quá áp đầu ra */
    CHG_LIB_ALARM_SHORT_CIRCUIT     = (1U << 4),  /* Ngắn mạch */
    CHG_LIB_ALARM_AC_UNDER_VOLT     = (1U << 5),  /* Áp đầu vào thấp */
    CHG_LIB_ALARM_OVER_CURR_OUT     = (1U << 6),  /* Quá dòng đầu ra */
    CHG_LIB_ALARM_FAN_FAULT         = (1U << 7),  /* Fan fault */
    CHG_LIB_ALARM_AC_OVER_VOLT      = (1U << 8),  /* AC input overvoltage */
    CHG_LIB_ALARM_AC_PHASE_LOSS     = (1U << 9),  /* AC input phase loss */
    /* PFC-related alarms (bits 16-20, leaving room for future expansion) */
    CHG_LIB_ALARM_PFC_OVERVOLT      = (1U << 16), /* Bus overvoltage */
    CHG_LIB_ALARM_PFC_OVERCURR      = (1U << 17), /* Input overcurrent */
    CHG_LIB_ALARM_PFC_IMBALANCE     = (1U << 18), /* Bus/phase imbalance */
    CHG_LIB_ALARM_FREQ_FAULT        = (1U << 19), /* Mains frequency fault */
    CHG_LIB_ALARM_PFC_FAULT         = (1U << 20), /* Vendor PFC fault summary */
    CHG_LIB_ALARM_OUTPUT_UNDER_VOLT = (1U << 21), /* DC output undervoltage warning */
    CHG_LIB_ALARM_OUTPUT_OVER_VOLT_WARN = (1U << 22), /* DC output overvoltage warning */
} CHG_LIB_AlarmFlag_t;

/* Advisory module warnings do not force FAULT or stop charging by themselves. */
#define CHG_LIB_ALARM_WARNING_MASK \
    ((uint32_t)CHG_LIB_ALARM_AC_UNDER_VOLT | \
     (uint32_t)CHG_LIB_ALARM_OUTPUT_UNDER_VOLT | \
     (uint32_t)CHG_LIB_ALARM_OUTPUT_OVER_VOLT_WARN)

#define CHG_LIB_ALARM_HAS_PROTECTION(flags) \
    ((((uint32_t)(flags)) & (~CHG_LIB_ALARM_WARNING_MASK)) != 0U)

/** Thống kê truyền thông cho 1 module (chuẩn hoá) */
typedef struct {
 uint32_t tx_count; /* Số frame đã gửi */
 uint32_t rx_count; /* Số response nhận được (OK) */
 uint32_t error_count;  /* Số response lỗi (NACK / parse fail) */
 uint32_t timeout_count; /* Số lần timeout giao tiếp */
 uint32_t recovery_count; /* Số lần recovery thành công */
} CHG_LIB_CommStats_t;

typedef struct {
    /* Address & Identity */
    uint8_t          addr;
    uint8_t          group;

    /* Basic Status */
    bool             enabled;
    bool             online;
    bool             running;
    CHG_LIB_State_t state;

    /* Output Parameters */
    float            voltage;
    float            current;
    float            current_limit;

    /* Temperatures */
    float            temp_dcdc;
    float            temp_ambient;
    float            temp_pfc;             /* PFC stage temperature (Maxwell: poll 0x0010) */

    /* AC Input (3-phase) */
    float            ac_phase_a_voltage;   /* V per phase (Maxwell: 0x000C, TonHe: M_C_3) */
    float            ac_phase_b_voltage;   /* V per phase */
    float            ac_phase_c_voltage;   /* V per phase */

    /* PFC Bus (DC link) */
    float            pfc_bus_pos_voltage;  /* Positive DC bus (Maxwell: 0x0008) */
    float            pfc_bus_neg_voltage;  /* Negative DC bus (Maxwell: 0x000A) */

    /* Power & Ratings */
    uint32_t         input_power;
    float            rated_power;          /* W (Maxwell: 0x0011) */
    float            rated_current;        /* A (Maxwell: 0x0012) */

    /* Input diagnostics (Maxwell only: 0x0005/0x004B were defined in
     * priv/chg_lib_protocol.h but never polled or exposed -- added
     * 2026-08-29 after cross-checking the driver against the vendor PDF's
     * full register table found this gap). 0 on any driver that doesn't
     * poll these (Lianming/TonHe leave them at their zero-init default). */
    float            input_dc_voltage;     /* V (Maxwell: 0x0005) */
    uint8_t          input_mode;           /* Maxwell 0x004B: 1=1-phase AC, 2=DC, 3=3-phase AC, 5=mode mismatch; 0=unknown/not polled */

    /* Alarms */
    uint32_t         alarm_status;         /* Raw alarm bits (for PC/Telemetry) */
    CHG_LIB_AlarmFlag_t alarm_flags;       /* Standardized flags (for Firmware logic) */
    uint8_t          pfc_fault;           /* PFC fault byte (TonHe: M_C_1 byte 8) */

    /* Timing */
    uint32_t         last_rx_tick;
    uint32_t         last_tx_tick;

    /* Communication Statistics */
    CHG_LIB_CommStats_t stats;
} CHG_LIB_ModuleView_t;

typedef struct {
    float    total_current;
    float    total_power_in;
    float    voltage;
    uint8_t  modules_online;
    uint8_t  modules_fault;
    bool     any_critical;
} CHG_LIB_SystemSummary_t;

typedef struct {
    const char *name;
    void    (*init)(void);
    void    (*deinit)(void);
    int8_t  (*add_module)(uint8_t addr, uint8_t group);
    bool    (*set_config)(uint8_t idx, float rated_current_a);
    void    (*remove_module)(uint8_t idx);
    bool    (*set_voltage)(uint8_t idx, float voltage_v);
    bool    (*set_current_limit)(uint8_t idx, float current_a);
    bool    (*start)(uint8_t idx);
    bool    (*stop)(uint8_t idx);
    void    (*set_voltage_all)(float voltage_v);
    void    (*set_current_limit_all)(float current_a);
    void    (*start_all)(void);
    void    (*stop_all)(void);
    void    (*emergency_stop)(void);
    void    (*process)(uint32_t now_tick);
    void    (*feed_frame)(uint32_t ext_id, const uint8_t *data, uint8_t dlc);
    void    (*get_system_summary)(CHG_LIB_SystemSummary_t *summary);
    uint8_t (*get_module_count)(void);
    bool    (*get_module_view)(uint8_t idx, CHG_LIB_ModuleView_t *view);
} CHG_LIB_DriverOps_t;

bool CHG_LIB_RegisterDriver(CHG_LIB_DriverId_t id, const CHG_LIB_DriverOps_t *ops);
bool CHG_LIB_SelectDriver(CHG_LIB_DriverId_t id);
CHG_LIB_DriverId_t CHG_LIB_GetActiveDriverId(void);
const char *CHG_LIB_GetActiveDriverName(void);

void CHG_LIB_Init(void);
int8_t CHG_LIB_AddModule(uint8_t addr, uint8_t group);
bool CHG_LIB_SetModuleConfig(uint8_t idx, float rated_current_a);
void CHG_LIB_RemoveModule(uint8_t idx);
bool CHG_LIB_SetVoltage(uint8_t idx, float voltage_v);
void CHG_LIB_SetVoltageAllEx(float voltage_v, CHG_LIB_TxSource_t source);
bool CHG_LIB_SetCurrentLimit(uint8_t idx, float current_a);
bool CHG_LIB_SetCurrentLimitEx(uint8_t idx, float current_a, CHG_LIB_TxSource_t source);
bool CHG_LIB_Start(uint8_t idx);
bool CHG_LIB_Stop(uint8_t idx);
void CHG_LIB_SetVoltageAll(float voltage_v);
void CHG_LIB_SetCurrentLimitAll(float current_a);
bool CHG_LIB_SetCurrentLimitAllEx(float current_a, CHG_LIB_TxSource_t source);
CHG_LIB_TxSource_t CHG_LIB_GetCommandSource(void);
uint32_t CHG_LIB_GetCurrentZeroRejectCount(void);
void CHG_LIB_RecordRejectedZero(CHG_LIB_TxSource_t source, CHG_LIB_CurrentPath_t path);
void CHG_LIB_StartAll(void);
void CHG_LIB_StopAll(void);
void CHG_LIB_EmergencyStop(void);
void CHG_LIB_Process(uint32_t now_tick);
void CHG_LIB_FeedCanFrame(uint32_t ext_id, const uint8_t *data, uint8_t dlc);
void CHG_LIB_GetSystemSummary(CHG_LIB_SystemSummary_t *summary);
uint8_t CHG_LIB_GetModuleCount(void);
bool CHG_LIB_GetModuleView(uint8_t idx, CHG_LIB_ModuleView_t *view);

#endif /* CHARGER_CORE_H */
