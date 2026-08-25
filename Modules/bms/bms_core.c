/**
 * @file    bms_core.c
 * @brief   BMS Driver Core - Implementation
 * @note    State machine, timeout watchdog, Ctrl_INFO transmission,
 *          BMS_Data_t management, and public API.
 *
 * References:
 *   - "CAN BMS_BB_PKG V1.0.md"
 *   - "bms_core.h"
 */

#include "bms_core.h"
#include "bms_can.h"
#include "bms_can.h"
#include "bms_protocol.h"
#include "bsp_can.h"
#include "main.h"
#include "main.h"
#include "debug_log.h"
#include <string.h>

/* ============== Private state ============== */

static volatile BMS_Data_t      g_bms_data;
static volatile BMS_View_t      g_bms_view;
static volatile BMS_State_t      g_bms_state;
static volatile uint32_t        g_last_valid_rx_tick;
static volatile uint32_t        g_last_ctrl_tx_tick;
static volatile uint32_t        g_last_data_log_tick;
static volatile BMS_ChargeCtrl_t g_charge_ctrl;
static volatile bool            g_initialized;
/* ISR → main flag: set in FeedFrame, cleared/logged in Process (no LOG in ISR) */
static volatile uint32_t        g_isr_rx_count;
static volatile bool            g_isr_new_data;

static bool has_any_valid_bms_data(void)
{
    if (g_bms_data.batt_st1.valid ||
        g_bms_data.cell_volt.valid ||
        g_bms_data.cell_temp.valid ||
        g_bms_data.alm_info.valid ||
        g_bms_data.batt_st2.valid ||
        g_bms_data.chg_request.valid ||
        g_bms_data.bms_sw_sta.valid ||
        g_bms_data.cell_temp_full.valid) {
        return true;
    }

    for (uint8_t i = 0U; i < BMS_MAX_CELL_VOLT_FRAMES; i++) {
        if (g_bms_data.cell_volt_full[i].valid) {
            return true;
        }
    }

    return false;
}

/* ============== Private: raw → physical ============== */

static void update_view_from_data(void)
{
    volatile BMS_View_t *v = &g_bms_view;

    /* BATT_ST1 */
    if (g_bms_data.batt_st1.valid) {
        v->batt_voltage = BMS_RAW_TO_VOLT(g_bms_data.batt_st1.raw_volt);
        v->batt_current = BMS_RAW_TO_CURR(g_bms_data.batt_st1.raw_curr);
        v->soc = g_bms_data.batt_st1.soc;
    }

    /* BATT_ST2 */
    if (g_bms_data.batt_st2.valid) {
        v->cap_remain  = g_bms_data.batt_st2.cap_remain;
        v->rate_cap    = g_bms_data.batt_st2.rate_cap;
        v->cycle_count = g_bms_data.batt_st2.cycle_count;
        v->soh         = g_bms_data.batt_st2.soh;
    }

    /* CELL_VOLT */
    if (g_bms_data.cell_volt.valid) {
        v->max_cell_volt = g_bms_data.cell_volt.max_cell_volt;
        v->min_cell_volt = g_bms_data.cell_volt.min_cell_volt;
        v->max_cv_no     = g_bms_data.cell_volt.max_cv_no;
        v->min_cv_no     = g_bms_data.cell_volt.min_cv_no;
    }

    /* CELL_TEMP: raw = Temp + 50, physical = raw - 50 */
    if (g_bms_data.cell_temp.valid) {
        v->max_cell_temp = BMS_RAW_TO_TEMP(g_bms_data.cell_temp.max_cell_temp);
        v->min_cell_temp = BMS_RAW_TO_TEMP(g_bms_data.cell_temp.min_cell_temp);
        v->avg_cell_temp = BMS_RAW_TO_TEMP(g_bms_data.cell_temp.avg_cell_temp);
        v->max_ct_no     = g_bms_data.cell_temp.max_ct_no;
        v->min_ct_no     = g_bms_data.cell_temp.min_ct_no;
    }

    /* CELL_TEMP_FULL: detailed temperature (relay, shunt, 6 cells) */
    if (g_bms_data.cell_temp_full.valid) {
        v->temp_relay   = BMS_RAW_TO_TEMP(g_bms_data.cell_temp_full.temp_relay);
        v->temp_shunt   = BMS_RAW_TO_TEMP(g_bms_data.cell_temp_full.temp_shunt);
        for (uint8_t i = 0U; i < 6U; i++) {
            v->cell_temp[i] = BMS_RAW_TO_TEMP(g_bms_data.cell_temp_full.cell_temp[i]);
        }
    }

    /* ChgRequest: Volt/Curr request = raw × 0.1 */
    if (g_bms_data.chg_request.valid) {
        v->chg_volt_request = BMS_RAW_TO_VOLT(g_bms_data.chg_request.batt_volt_req);
        v->chg_curr_request = BMS_RAW_TO_REQ_CURR(g_bms_data.chg_request.batt_curr_req);
    }

    /* BmsSwSta */
    if (g_bms_data.bms_sw_sta.valid) {
        v->charge_relay_closed    = g_bms_data.bms_sw_sta.charge_sta;
        v->discharge_relay_closed = g_bms_data.bms_sw_sta.discharge_sta;
    }
}

/* ============== Private: map alarm severity ============== */

static BMS_AlarmFlag_t map_alarm_field(uint8_t sev, BMS_AlarmFlag_t flag)
{
    /* Severity 1 = warning; 2 or 3 = fault. Accept any severity >= 1 */
    if (sev >= 1U) {
        return flag;
    }
    return BMS_ALARM_NONE;
}

static void update_alarm_flags(void)
{
    BMS_AlarmFlag_t flags = BMS_ALARM_NONE;
    const volatile BMS_AlmInfo_t *a = &g_bms_data.alm_info;

    if (!a->valid) {
        g_bms_view.alarm_flags = flags;
        return;
    }

    flags |= map_alarm_field(a->low_pack_volt,      BMS_ALARM_LOW_PACK_VOLT);
    flags |= map_alarm_field(a->low_cell_volt,      BMS_ALARM_LOW_CELL_VOLT);
    flags |= map_alarm_field(a->high_pack_volt,     BMS_ALARM_HIGH_PACK_VOLT);
    flags |= map_alarm_field(a->high_cell_volt,     BMS_ALARM_HIGH_CELL_VOLT);
    flags |= map_alarm_field(a->temp_cell_high_chg,  BMS_ALARM_TEMP_HIGH_CHG);
    flags |= map_alarm_field(a->temp_cell_high_dchg, BMS_ALARM_TEMP_HIGH_DCHG);
    flags |= map_alarm_field(a->temp_cell_low_chg,  BMS_ALARM_TEMP_LOW_CHG);
    flags |= map_alarm_field(a->temp_cell_low_dchg, BMS_ALARM_TEMP_LOW_DCHG);
    flags |= map_alarm_field(a->temp_relay_high,    BMS_ALARM_TEMP_RELAY_HIGH);
    flags |= map_alarm_field(a->over_chg_curr,      BMS_ALARM_OVER_CHG_CURR);
    flags |= map_alarm_field(a->over_dchg_curr,     BMS_ALARM_OVER_DCHG_CURR);
    flags |= map_alarm_field(a->cell_volt_diff,    BMS_ALARM_CELL_VOLT_DIFF);
    flags |= map_alarm_field(a->low_soc,            BMS_ALARM_LOW_SOC);

    g_bms_view.alarm_flags = flags;
}

/* ============== Private: CAN TX wrapper (forward decl) ============== */
#include "bms_can.h"

/* ============== Private: send Ctrl_INFO ============== */

static void transmit_ctrl_info(void)
{
    BSP_CAN_Frame_t frame;
    BMS_CtrlInfo_t  ctrl;

    frame.ext_id = BMS_ID_CTRL_INFO;
    frame.dlc    = 8U;

    /* Mask: bit0=charge ctrl allowed, bit1=discharge ctrl allowed */
    ctrl.mask_code = 0U;
    if (g_charge_ctrl.allow_charge) {
        ctrl.mask_code |= 0x01U;
    }
    if (g_charge_ctrl.allow_discharge) {
        ctrl.mask_code |= 0x02U;
    }
    ctrl.chg_sw  = g_charge_ctrl.allow_charge ? 1U : 0U;
    ctrl.dchg_sw = g_charge_ctrl.allow_discharge ? 1U : 0U;

    BMS_BuildCtrlInfo(frame.data, &ctrl);
    (void)BMS_CAN_Transmit(frame.ext_id, frame.data, frame.dlc);
}

/* ============== Private: CAN TX wrapper ============== */



/* ============== Public API ============== */

void BMS_Init(void)
{
    memset((void *)&g_bms_data, 0, sizeof(g_bms_data));
    memset((void *)&g_bms_view, 0, sizeof(g_bms_view));
    memset((void *)&g_charge_ctrl, 0, sizeof(g_charge_ctrl));

    g_bms_state          = BMS_STATE_OFFLINE;
    g_last_valid_rx_tick = HAL_GetTick();
    g_last_ctrl_tx_tick  = 0U;
    g_last_data_log_tick = 0U;
    g_initialized        = true;

    LOG("BMS_Init: driver ready.\r\n");
}

void BMS_FeedFrame(uint32_t ext_id, uint32_t std_id,
                   const uint8_t *data, uint8_t dlc)
{
    if (!g_initialized) {
        return;
    }

    /* Parse into g_bms_data. Unknown or short frames must not refresh the watchdog.
     * NOTE: No LOG here — this runs in ISR context (HAL_FDCAN_RxFifo0Callback). */
    BMS_Data_t *bms_data_mut = (BMS_Data_t *)&g_bms_data;
    if (!BMS_ParseFrame(ext_id, std_id, data, dlc, bms_data_mut)) {
        return;
    }

    /* Mark as online on first valid frame — ISR-safe, no LOG */
    g_last_valid_rx_tick = HAL_GetTick();
    g_isr_rx_count++;
    g_isr_new_data = true;

    /* Refresh cached view — ISR context, keep minimal (no LOG) */
    update_view_from_data();
    update_alarm_flags();
}

void BMS_Process(uint32_t now_tick)
{
    if (!g_initialized) {
        return;
    }

    /* Critical section: snapshot last_rx_tick atomically (ISR may update it) */
    uint32_t last_rx_snapshot;
    __disable_irq();
    last_rx_snapshot = g_last_valid_rx_tick;
    __enable_irq();

    /* Handle uint32_t underflow when now_tick < g_last_valid_rx_tick
     * This can happen when:
     * 1. Actual tick overflow (every ~49 days for 1ms tick)
     * 2. ISR updates g_last_valid_rx_tick AFTER BMS_Process is called in the same tick
     *    (e.g., BMS_Process runs at tick N, then CAN ISR updates tick to N+X where X>0)
     * In case #2, we should treat elapsed as 0 (recent data received) */
    uint32_t elapsed;
    if (now_tick >= last_rx_snapshot) {
        elapsed = now_tick - last_rx_snapshot;
    } else {
        /* now_tick < last_rx_snapshot: likely case #2 above, treat as recent */
        elapsed = 0U;
    }

    /* DEBUG: Log timeout calculation */
    /* DEBUG: only log on state changes, not every call */
    /* LOG("BMS_Process: elapsed=%lu (timeout=%u) last_rx=%lu now=%lu state=%d\r\n", */
    /*     elapsed, BMS_OFFLINE_TIMEOUT_MS, g_last_valid_rx_tick, now_tick, (int)g_bms_state); */

    /* ---- State Machine ---- */
    if (g_bms_state == BMS_STATE_OFFLINE) {
        /* Wait for first frame */
        if (elapsed < BMS_OFFLINE_TIMEOUT_MS) {
            /* Still offline but receiving — could transition */
        } else {
            /* Still no data after timeout — stay offline */
        }
        /* Transition to ONLINE once we have data */
        if (has_any_valid_bms_data()) {
            g_bms_state = BMS_STATE_ONLINE;
            g_bms_view.online = true;
            LOG("BMS: ONLINE (was OFFLINE, now has data)\r\n");
        }
    }
    else if (g_bms_state == BMS_STATE_ONLINE) {
        if (elapsed >= BMS_OFFLINE_TIMEOUT_MS) {
            g_bms_state = BMS_STATE_OFFLINE;
            g_bms_view.online = false;
            g_bms_view.alarm_flags = BMS_ALARM_BMS_OFFLINE;
            /* Clear all parsed data so stale values are not used */
            memset((void *)&g_bms_data, 0, sizeof(g_bms_data));
            LOG("BMS: OFFLINE (timeout after %lu ms from tick %lu, now %lu)\r\n",
                (unsigned long)elapsed, (unsigned long)last_rx_snapshot, (unsigned long)now_tick);
        } else {
            /* Connectivity is based on any valid BMS frame.  STALE is a
             * data-quality warning, not an offline condition. */
            g_bms_view.online = true;
            if (elapsed >= BMS_STALE_THRESHOLD_MS) {
                g_bms_view.alarm_flags |= BMS_ALARM_STALE_DATA;
            }
            /* Only critical alarms (not STALE_DATA) transition to FAULT */
            BMS_AlarmFlag_t critical_mask = (
                BMS_ALARM_HIGH_CELL_VOLT | BMS_ALARM_OVER_CHG_CURR |
                BMS_ALARM_TEMP_HIGH_CHG  | BMS_ALARM_BMS_OFFLINE);
            if (g_bms_view.alarm_flags & critical_mask) {
                g_bms_state = BMS_STATE_FAULT;
                LOG("BMS: FAULT (critical alarm 0x%08lX)\r\n",
                    (unsigned long)(g_bms_view.alarm_flags & critical_mask));
            }
        }
    }
    else if (g_bms_state == BMS_STATE_FAULT) {
        if (elapsed >= BMS_OFFLINE_TIMEOUT_MS) {
            /* A faulted BMS can still lose communication.  Connectivity
             * timeout must always win over the previous alarm state. */
            g_bms_state = BMS_STATE_OFFLINE;
            g_bms_view.online = false;
            g_bms_view.alarm_flags = BMS_ALARM_BMS_OFFLINE;
            memset((void *)&g_bms_data, 0, sizeof(g_bms_data));
            LOG("BMS: OFFLINE (timeout while faulted)\r\n");
        } else {
            g_bms_view.online = true;
            if (elapsed >= BMS_STALE_THRESHOLD_MS) {
                g_bms_view.alarm_flags |= BMS_ALARM_STALE_DATA;
            }
        }

        /* Fault recovery: auto-recover when alarms clear and data resumes */
        if (g_bms_state == BMS_STATE_FAULT &&
            (g_bms_view.alarm_flags == BMS_ALARM_NONE) &&
            (elapsed < BMS_STALE_THRESHOLD_MS)) {
            g_bms_state = BMS_STATE_ONLINE;
            LOG("BMS: Recovered from FAULT -> ONLINE\r\n");
        }
    }

    /* ---- Periodic Ctrl_INFO transmission ---- */
    if ((now_tick - g_last_ctrl_tx_tick) >= BMS_CTRL_TX_INTERVAL_MS) {
        g_last_ctrl_tx_tick = now_tick;
        transmit_ctrl_info();
    }

    /* ---- Throttled snapshot LOG (1s) — moved out of ISR ---- */
    if (g_isr_new_data) {
        uint32_t last_log;
        __disable_irq();
        last_log = g_last_data_log_tick;
        __enable_irq();
        if ((now_tick - last_log) >= 1000U) {
            __disable_irq();
            g_last_data_log_tick = now_tick;
            g_isr_new_data = false;
            __enable_irq();
            /* Snapshot atomically for logging */
            BMS_View_t snap;
            __disable_irq();
            snap = *(BMS_View_t *)&g_bms_view;
            __enable_irq();
            int batt_v_x10 = (int)(snap.batt_voltage * 10.0f);
            int batt_i_x10 = (int)(snap.batt_current * 10.0f);
            int temp_max_x10 = (int)(snap.max_cell_temp * 10.0f);
            int temp_min_x10 = (int)(snap.min_cell_temp * 10.0f);
            int req_v_x10 = (int)(snap.chg_volt_request * 10.0f);
            int req_i_x10 = (int)(snap.chg_curr_request * 10.0f);
            int batt_i_abs = (batt_i_x10 < 0) ? -batt_i_x10 : batt_i_x10;
            int temp_max_abs = (temp_max_x10 < 0) ? -temp_max_x10 : temp_max_x10;
            int temp_min_abs = (temp_min_x10 < 0) ? -temp_min_x10 : temp_min_x10;
            int req_v_abs = (req_v_x10 < 0) ? -req_v_x10 : req_v_x10;
            int req_i_abs = (req_i_x10 < 0) ? -req_i_x10 : req_i_x10;
            LOG("[BMS SNAP] online=%u V=%d.%d I=%s%d.%d SOC=%u%% "
                "Cell=%u/%u mV Temp=%s%d.%d/%s%d.%d C Relay=%u/%u "
                "Req=%s%d.%dV/%s%d.%dA alarms=0x%08lX\r\n",
                snap.online ? 1U : 0U,
                batt_v_x10 / 10, batt_v_x10 % 10,
                (batt_i_x10 < 0) ? "-" : "", batt_i_abs / 10, batt_i_abs % 10,
                snap.soc,
                snap.max_cell_volt, snap.min_cell_volt,
                (temp_max_x10 < 0) ? "-" : "", temp_max_abs / 10, temp_max_abs % 10,
                (temp_min_x10 < 0) ? "-" : "", temp_min_abs / 10, temp_min_abs % 10,
                snap.charge_relay_closed ? 1U : 0U,
                snap.discharge_relay_closed ? 1U : 0U,
                (req_v_x10 < 0) ? "-" : "", req_v_abs / 10, req_v_abs % 10,
                (req_i_x10 < 0) ? "-" : "", req_i_abs / 10, req_i_abs % 10,
                (unsigned long)snap.alarm_flags);
        }
    }

    g_bms_view.state       = g_bms_state;
    __disable_irq();
    g_bms_view.last_rx_tick = g_last_valid_rx_tick;
    __enable_irq();
}

void BMS_SendCtrlInfo(const BMS_ChargeCtrl_t *ctrl)
{
    if (ctrl == NULL) {
        return;
    }
    g_charge_ctrl.allow_charge    = ctrl->allow_charge;
    g_charge_ctrl.allow_discharge = ctrl->allow_discharge;

    /* Transmit immediately */
    transmit_ctrl_info();
}

bool BMS_IsOnline(void)
{
    /* ONLINE means the watchdog has seen any valid BMS frame recently.
     * A BMS may still be in BMS_STATE_FAULT while remaining connected. */
    return g_bms_view.online;
}

bool BMS_HasCriticalAlarm(void)
{
    /* STALE is a connectivity warning, not a critical offline condition. */
    BMS_AlarmFlag_t crit = (
        BMS_ALARM_HIGH_CELL_VOLT  |
        BMS_ALARM_TEMP_HIGH_CHG   |
        BMS_ALARM_OVER_CHG_CURR   |
        BMS_ALARM_BMS_OFFLINE
    );
    return ((g_bms_view.alarm_flags & crit) != 0U);
}

void BMS_GetView(BMS_View_t *view)
{
    if (view == NULL) {
        return;
    }
    __disable_irq();
    *view = *(BMS_View_t *)&g_bms_view;
    __enable_irq();
}

bool BMS_ShouldCloseChargeRelay(void)
{
    if (!BMS_IsOnline()) {
        return false;
    }

    BMS_View_t snap;
    __disable_irq();
    snap = *(BMS_View_t *)&g_bms_view;
    __enable_irq();

    /* Charge relay in BMS must be closed */
    if (!snap.charge_relay_closed) {
        return false;
    }

    (void)snap;
    return true;
}





