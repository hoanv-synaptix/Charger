/**
 * @file    bms_core.h
 * @brief   BMS Driver Core - State Machine, Timeout, and Public API
 * @note    Handles BMS data, timeout detection, Ctrl_INFO transmission.
 *
 * Architecture:
 *   ┌──────────────────────────────────────┐
 *   │            bms_core.c/h               │
 *   │  - BMS state machine (online/offline) │
 *   │  - Timeout tracking                   │
 *   │  - Alarm debouncing                   │
 *   │  - Ctrl_INFO TX scheduling           │
 *   │  - BMS_Data_t management              │
 *   └──────────────┬────────────────────────┘
 *                  │ uses
 *                  ↓
 *   ┌──────────────────────────────────────┐
 *   │           bms_protocol.c/h             │
 *   │  - CAN frame parsing                  │
 *   │  - Ctrl_INFO frame building           │
 *   └──────────────────────────────────────┘
 *
 * Usage:
 *   1. BMS_Init()     — called once at startup
 *   2. BMS_FeedFrame()— called from CAN2 RX ISR
 *   3. BMS_Process()  — called periodically in main loop
 *
 * Design Patterns:
 *   - State Machine: OFFLINE → ONLINE → (FAULT?)
 *   - Timeout Watchdog: track last RX tick per message type
 */

#ifndef BMS_CORE_H
#define BMS_CORE_H

#include <stdbool.h>
#include <stdint.h>
#include "bms_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============== Configuration ============== */

#define BMS_OFFLINE_TIMEOUT_MS    5000U   /* BMS offline after 5s no data */
#define BMS_STALE_THRESHOLD_MS    2000U   /* Data stale after 2s */
#define BMS_CTRL_TX_INTERVAL_MS   500U    /* Send Ctrl_INFO every 500ms */
#define BMS_MIN_VOLT_MV           2000U   /* Min cell voltage (mV) */
#define BMS_MAX_VOLT_MV           4500U   /* Max cell voltage (mV) */
#define BMS_MAX_TEMP_DEGC         60U     /* Max allowed cell temp (°C) */
#define BMS_MIN_TEMP_DEGC         (-10)   /* Min allowed cell temp (°C) */
#define BMS_CHARGE_VOLT_LIMIT_PCT  90U    /* Relay closes at 90% of req voltage */

/* ============== BMS State ============== */

typedef enum {
    BMS_STATE_OFFLINE = 0,   /* No valid data received yet */
    BMS_STATE_ONLINE,        /* BMS responding normally */
    BMS_STATE_FAULT,         /* BMS reported critical alarm */
} BMS_State_t;

/* ============== BMS Alarms (standardized flags) ============== */

typedef uint32_t BMS_AlarmFlag_t;

#define BMS_ALARM_NONE              (0U)
#define BMS_ALARM_LOW_PACK_VOLT      (1U << 0)
#define BMS_ALARM_LOW_CELL_VOLT      (1U << 1)
#define BMS_ALARM_HIGH_PACK_VOLT     (1U << 2)
#define BMS_ALARM_HIGH_CELL_VOLT     (1U << 3)
#define BMS_ALARM_TEMP_HIGH_CHG      (1U << 4)
#define BMS_ALARM_TEMP_HIGH_DCHG     (1U << 5)
#define BMS_ALARM_TEMP_LOW_CHG       (1U << 6)
#define BMS_ALARM_TEMP_LOW_DCHG      (1U << 7)
#define BMS_ALARM_TEMP_RELAY_HIGH    (1U << 8)
#define BMS_ALARM_OVER_CHG_CURR      (1U << 9)
#define BMS_ALARM_OVER_DCHG_CURR     (1U << 10)
#define BMS_ALARM_CELL_VOLT_DIFF     (1U << 11)
#define BMS_ALARM_LOW_SOC            (1U << 12)
#define BMS_ALARM_BMS_OFFLINE        (1U << 13)
#define BMS_ALARM_STALE_DATA         (1U << 14)

/* ============== BMS View (public data snapshot) ============== */

typedef struct {
    BMS_State_t state;

    /* Battery status */
    float  batt_voltage;      /* V */
    float  batt_current;      /* A */
    uint8_t soc;             /* % */
    uint16_t cap_remain;    /* Ah × 0.1 */
    uint16_t rate_cap;       /* Ah × 0.1 */
    uint16_t cycle_count;
    uint8_t  soh;            /* % */

    /* Cell status */
    uint16_t max_cell_volt;  /* mV */
    uint16_t min_cell_volt;  /* mV */
    float   max_cell_temp;   /* °C, physical */
    float   min_cell_temp;   /* °C, physical */
    float   avg_cell_temp;   /* °C, physical */
    uint8_t  max_cv_no;      /* 1-based cell number */
    uint8_t  min_cv_no;      /* 1-based cell number */
    uint8_t  max_ct_no;      /* 1-based temp sensor number */
    uint8_t  min_ct_no;      /* 1-based temp sensor number */

    /* Detailed temperatures from CELL_TEMP_FULL */
    float    temp_relay;      /* °C, physical */
    float    temp_shunt;      /* °C, physical */
    float    cell_temp[6];    /* °C, 6 cell temperatures */

    /* Relay status */
    bool charge_relay_closed;
    bool discharge_relay_closed;

    /* Charging request (from BMS) */
    float  chg_volt_request;  /* V */
    float  chg_curr_request;  /* A */

    /* Alarms */
    BMS_AlarmFlag_t alarm_flags;

    /* Timing */
    uint32_t last_rx_tick;
    bool     online;
} BMS_View_t;

/* ============== Config for charging ============== */

typedef struct {
    bool allow_charge;
    bool allow_discharge;
} BMS_ChargeCtrl_t;

/* ============== Public API ============== */

/**
 * @brief  Initialize BMS driver (call once at startup)
 */
void BMS_Init(void);

/**
 * @brief  Feed a CAN frame from BMS into the driver.
 *         Call from CAN2 RX ISR or from App_BMS_RxCallback().
 * @param  ext_id   Extended ID (0 if standard frame)
 * @param  std_id   Standard ID (0 if extended frame)
 * @param  data     8-byte payload
 * @param  dlc      Data length
 */
void BMS_FeedFrame(uint32_t ext_id, uint32_t std_id,
                   const uint8_t *data, uint8_t dlc);

/**
 * @brief  Periodic process — timeout check, state machine, Ctrl_INFO TX.
 *         Call every ~100ms from main loop.
 * @param  now_tick  Current HAL_GetTick() value
 */
void BMS_Process(uint32_t now_tick);

/**
 * @brief  Send Ctrl_INFO to BMS (enable/disable charging control)
 * @param  ctrl   Control settings
 */
void BMS_SendCtrlInfo(const BMS_ChargeCtrl_t *ctrl);

/**
 * @brief  Check if BMS is online and healthy
 */
bool BMS_IsOnline(void);

/**
 * @brief  Check if any critical alarm is active
 */
bool BMS_HasCriticalAlarm(void);

/**
 * @brief  Get BMS data snapshot (call any time, returns cached values)
 * @param  view  Pointer to fill with current BMS data
 */
void BMS_GetView(BMS_View_t *view);

/**
 * @brief  Decide if charging relay should be closed based on BMS state.
 *         Returns true when: BMS online, charge relay closed, voltage >90% req.
 */
bool BMS_ShouldCloseChargeRelay(void);

/* ============== Alarm Check API ============== */

/**
 * @brief  Map raw ALM_INFO severity (0..3) to alarm flag
 */
BMS_AlarmFlag_t BMS_MapAlmSeverity(uint8_t severity, BMS_AlarmFlag_t base_flag);

#ifdef __cplusplus
}
#endif

#endif /* BMS_CORE_H */
