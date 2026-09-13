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
#include "bsp_sys.h"
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
static volatile bool            g_bms_data_stale;
/* ISR → main flag: set in FeedFrame, cleared/logged in Process (no LOG in ISR) */
static volatile uint32_t        g_rx_count;
/* Main-loop flag: set when a valid frame is consumed, cleared after logging. */
static volatile bool            g_new_data;
static volatile uint32_t        g_valid_rx_count[BMS_FRAME_MAX];
static volatile uint32_t        g_unknown_id_count;
static volatile uint32_t        g_invalid_dlc_count;
static volatile uint32_t        g_argument_reject_count;

/**
 * @brief  Elapsed time since a tick timestamp, tolerant of both the ISR/main
 *         snapshot race (last_tick briefly ahead of now by a few ms) and true
 *         32-bit tick overflow (~49.7 days of uptime).
 * @note   BUGFIX BUG-02: the previous `now >= last ? now-last : 0` used a
 *         plain unsigned comparison, which cannot tell "last is a few ms
 *         ahead due to a race" apart from "the tick counter wrapped and last
 *         is actually far in the past" -- both look like now < last. It
 *         silently returned 0 (not stale) for a real wraparound-while-stale
 *         case too. Casting the unsigned wraparound difference to signed
 *         handles both correctly: a genuine ahead-of-now snapshot yields a
 *         small negative diff (clamped to 0), while a real overflow still
 *         yields the true positive elapsed time via modular arithmetic. */
static uint32_t bms_tick_elapsed(uint32_t now_tick, uint32_t last_tick)
{
    int32_t diff = (int32_t)(now_tick - last_tick);
    return (diff < 0) ? 0U : (uint32_t)diff;
}

/**
 * @brief  Alarms serious enough to fault the BMS / refuse the charge relay.
 * @note   Single source of truth for both BMS_Process()'s FAULT transition
 *         and BMS_HasCriticalAlarm() -- these two previously duplicated the
 *         mask inline and could silently drift apart.
 *         HIGH_PACK_VOLT and TEMP_LOW_CHG added: pack-level overvoltage can
 *         occur without any single cell crossing HIGH_CELL_VOLT (e.g. even
 *         cell imbalance masking a real pack OV), and charging at low
 *         temperature risks lithium plating -- both are charging-safety
 *         critical, not just cell-level bookkeeping. */
static BMS_AlarmFlag_t bms_critical_alarm_mask(void)
{
    return (BMS_AlarmFlag_t)(
        BMS_ALARM_HIGH_CELL_VOLT  |
        BMS_ALARM_HIGH_PACK_VOLT  |
        BMS_ALARM_OVER_CHG_CURR   |
        BMS_ALARM_TEMP_HIGH_CHG   |
        BMS_ALARM_TEMP_LOW_CHG    |
        BMS_ALARM_BMS_OFFLINE);
}

/**
 * @brief  True once any registered BMS frame type has ever been parsed.
 * @note   MNT-01 (docs/AUDIT_Findings.md): this used to be a hand-maintained
 *         OR-chain of every BMS_Data_t sub-struct's `.valid` field, one more
 *         place to remember when wiring up a new frame type. BMS_ParseFrame()
 *         already stamps last_rx_tick[type] for every registered handler
 *         (see g_handlers[] in bms_protocol.c) regardless of frame type, so
 *         this can just read that instead -- adding a new frame to the
 *         dispatch table now makes it count here for free. */
static bool has_any_valid_bms_data(void)
{
    for (int i = 0; i < BMS_FRAME_MAX; i++) {
        if (g_bms_data.last_rx_tick[i] != 0U) {
            return true;
        }
    }
    return false;
}

static uint32_t bms_stale_timeout_ms(BMS_FrameType_t type)
{
    switch (type) {
        case BMS_FRAME_BATT_ST1:     return 200U;
        case BMS_FRAME_BATT_ST2:     return 1000U;
        case BMS_FRAME_CELL_VOLT:    return 500U;
        case BMS_FRAME_CELL_TEMP:    return 2000U;
        case BMS_FRAME_CHG_REQUEST:  return 2000U;
        default:                     return 0U;
    }
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

static void map_alarm_field(uint8_t sev, BMS_AlarmFlag_t flag,
                            BMS_AlarmFlag_t *warning_flags,
                            BMS_AlarmFlag_t *fault_flags)
{
    if (sev == 1U) {
        *warning_flags |= flag;
    } else if (sev >= 2U) {
        /* Only fault/severe BMS reports may affect charging safety. */
        *fault_flags |= flag;
    }
}

static void update_alarm_flags(void)
{
    /* BMS_ALARM_BMS_OFFLINE is owned by BMS_Process() (main loop), not by
     * this frame update. This used to be a full overwrite of
     * g_bms_view.alarm_flags, so every incoming ALM_INFO frame silently clobbered
     * whichever bit BMS_Process had set -- not a rare timing race, a
     * guaranteed clobber on every frame. Preserve it explicitly. */
    BMS_AlarmFlag_t warning_flags = BMS_ALARM_NONE;
    BMS_AlarmFlag_t fault_flags = BMS_ALARM_NONE;
    const volatile BMS_AlmInfo_t *a = &g_bms_data.alm_info;
    const BMS_AlarmFlag_t preserve_mask = BMS_ALARM_BMS_OFFLINE;

    if (a->valid) {
        map_alarm_field(a->low_pack_volt,      BMS_ALARM_LOW_PACK_VOLT, &warning_flags, &fault_flags);
        map_alarm_field(a->low_cell_volt,      BMS_ALARM_LOW_CELL_VOLT, &warning_flags, &fault_flags);
        map_alarm_field(a->high_pack_volt,     BMS_ALARM_HIGH_PACK_VOLT, &warning_flags, &fault_flags);
        map_alarm_field(a->high_cell_volt,     BMS_ALARM_HIGH_CELL_VOLT, &warning_flags, &fault_flags);
        map_alarm_field(a->temp_cell_high_chg,  BMS_ALARM_TEMP_HIGH_CHG, &warning_flags, &fault_flags);
        map_alarm_field(a->temp_cell_high_dchg, BMS_ALARM_TEMP_HIGH_DCHG, &warning_flags, &fault_flags);
        map_alarm_field(a->temp_cell_low_chg,  BMS_ALARM_TEMP_LOW_CHG, &warning_flags, &fault_flags);
        map_alarm_field(a->temp_cell_low_dchg, BMS_ALARM_TEMP_LOW_DCHG, &warning_flags, &fault_flags);
        map_alarm_field(a->temp_relay_high,    BMS_ALARM_TEMP_RELAY_HIGH, &warning_flags, &fault_flags);
        map_alarm_field(a->over_chg_curr,      BMS_ALARM_OVER_CHG_CURR, &warning_flags, &fault_flags);
        map_alarm_field(a->over_dchg_curr,     BMS_ALARM_OVER_DCHG_CURR, &warning_flags, &fault_flags);
        map_alarm_field(a->cell_volt_diff,    BMS_ALARM_CELL_VOLT_DIFF, &warning_flags, &fault_flags);
        map_alarm_field(a->low_soc,            BMS_ALARM_LOW_SOC, &warning_flags, &fault_flags);
    }

    g_bms_view.warning_flags = warning_flags;
    g_bms_view.alarm_flags = (BMS_AlarmFlag_t)(fault_flags | (g_bms_view.alarm_flags & preserve_mask));
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
    /* BUGFIX DES-01: this used to seed with the current tick, as if a frame
     * had just been received. 0 is the documented "never received anything"
     * sentinel (see e.g. CHG_LIB module views' own last_rx_tick==0 check) --
     * seeding with "now" made BMS_View_t.last_rx_tick briefly claim data had
     * just arrived even at boot, before the first real frame. */
    g_last_valid_rx_tick = 0U;
    g_last_ctrl_tx_tick  = 0U;
    g_last_data_log_tick = 0U;
    g_bms_data_stale     = false;
    g_rx_count           = 0U;
    g_new_data           = false;
    memset((void *)g_valid_rx_count, 0, sizeof(g_valid_rx_count));
    g_unknown_id_count   = 0U;
    g_invalid_dlc_count  = 0U;
    g_argument_reject_count = 0U;
    g_initialized        = true;

    LOG("BMS_Init: driver ready.\r\n");
}

void BMS_FeedFrame(uint32_t ext_id, uint32_t std_id,
                   const uint8_t *data, uint8_t dlc)
{
    if (!g_initialized) {
        return;
    }

    /* Parse in main context. Unknown or short frames must not refresh the
     * watchdog and are counted for field diagnostics. */
    BMS_Data_t *bms_data_mut = (BMS_Data_t *)&g_bms_data;
    BMS_ParseRejectReason_t reject_reason;
    BMS_FrameType_t frame_type;
    if (!BMS_ParseFrameEx(ext_id, std_id, data, dlc, bms_data_mut,
                          &reject_reason, &frame_type)) {
        if (reject_reason == BMS_PARSE_REJECT_UNKNOWN_ID) {
            g_unknown_id_count++;
        } else if (reject_reason == BMS_PARSE_REJECT_DLC) {
            g_invalid_dlc_count++;
        } else {
            g_argument_reject_count++;
        }
        return;
    }

    /* Mark as online on first valid frame. */
    g_last_valid_rx_tick = BSP_GetTick();
    if (frame_type < BMS_FRAME_MAX) g_valid_rx_count[frame_type]++;
    g_rx_count++;
    g_new_data = true;

    /* Refresh cached view in main context, where float conversion is allowed. */
    update_view_from_data();
    update_alarm_flags();
}

void BMS_Process(uint32_t now_tick)
{
    if (!g_initialized) {
        return;
    }

    /* Snapshot shared state before evaluating timeout conditions. */
    uint32_t last_rx_snapshot;
    BSP_EnterCritical();
    last_rx_snapshot = g_last_valid_rx_tick;
    BSP_ExitCritical();

    /* elapsed since last valid RX -- see bms_tick_elapsed() for why this
     * isn't a plain `now >= last ? now-last : 0` (BUG-02). This can go
     * negative-then-clamped either because the ISR updated last_rx_snapshot
     * a few ms after `now_tick` was captured by the caller, or because the
     * tick counter wrapped (~49.7 days uptime). */
    uint32_t elapsed = bms_tick_elapsed(now_tick, last_rx_snapshot);

    uint32_t last_rx_frames[BMS_FRAME_MAX];
    BSP_EnterCritical();
    memcpy(last_rx_frames, (const void*)g_bms_data.last_rx_tick, sizeof(last_rx_frames));
    BSP_ExitCritical();

    bool is_stale = false;
    for (int i = 0; i < BMS_FRAME_MAX; i++) {
        uint32_t timeout = bms_stale_timeout_ms((BMS_FrameType_t)i);
        if (timeout == 0U || last_rx_frames[i] == 0U) {
            continue; /* Optional/event frames and never-seen frames do not stale. */
        }
        uint32_t f_elapsed = bms_tick_elapsed(now_tick, last_rx_frames[i]);
        if (f_elapsed >= timeout) {
            is_stale = true;
            break;
        }
    }

    /* Event-triggered ALM_INFO timeout: per vendor PDF (§5.4 0x07F4 cycle is 100ms
     * when active, but "if there is no alarm information, it will not be sent").
     * When BMS alarm condition recovers, BMS stops sending 0x07F4. If no frame
     * arrives for BMS_ALM_INFO_TIMEOUT_MS, clear cached alarm data so alarms
     * auto-recover and fault code resets to 0000. */
    if (g_bms_data.alm_info.valid && last_rx_frames[BMS_FRAME_ALM_INFO] != 0U) {
        uint32_t alm_elapsed = bms_tick_elapsed(now_tick, last_rx_frames[BMS_FRAME_ALM_INFO]);
        if (alm_elapsed >= BMS_ALM_INFO_TIMEOUT_MS) {
            BSP_EnterCritical();
            g_bms_data.alm_info.valid = false;
            memset((void*)&g_bms_data.alm_info, 0, sizeof(g_bms_data.alm_info));
            BSP_ExitCritical();
            update_alarm_flags();
        }
    }

    /* ---- State Machine ---- */
    if (g_bms_state == BMS_STATE_OFFLINE) {
        /* Transition to ONLINE as soon as any valid frame has been parsed and
         * elapsed is within timeout. */
        if (has_any_valid_bms_data() && elapsed < BMS_OFFLINE_TIMEOUT_MS) {
            g_bms_state = BMS_STATE_ONLINE;
            g_bms_view.online = true;
            BSP_EnterCritical();
            g_bms_data_stale = false;
            g_bms_view.alarm_flags &= (BMS_AlarmFlag_t)~BMS_ALARM_BMS_OFFLINE;
            BSP_ExitCritical();
            LOG("BMS: ONLINE (was OFFLINE, now has data)\r\n");
        }
    }
    else if (g_bms_state == BMS_STATE_ONLINE) {
        if (elapsed >= BMS_OFFLINE_TIMEOUT_MS) {
            g_bms_state = BMS_STATE_OFFLINE;
            /* Retain last-known good telemetry in g_bms_view (SOC, pack voltage,
             * capacity, temperatures) so HMI and PC app do not reset to 0
             * while OFFLINE. Only flag offline status and raise alarm. */
            g_bms_view.online = false;
            BSP_EnterCritical();
            g_bms_data_stale = false;
            g_bms_view.alarm_flags |= BMS_ALARM_BMS_OFFLINE;
            BSP_ExitCritical();
            LOG("BMS: OFFLINE (timeout after %lu ms from tick %lu, now %lu)\r\n",
                (unsigned long)elapsed, (unsigned long)last_rx_snapshot, (unsigned long)now_tick);
        } else {
            /* Connectivity is based on any valid BMS frame. Staleness is a
             * separate data-quality state, not an alarm flag. */
            g_bms_view.online = true;
            BSP_EnterCritical();
            g_bms_data_stale = is_stale;
            BSP_ExitCritical();
            /* Only CAN-reported critical alarms transition to FAULT. */
            BMS_AlarmFlag_t critical_mask = bms_critical_alarm_mask();
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
            BSP_EnterCritical();
            g_bms_data_stale = false;
            g_bms_view.alarm_flags |= BMS_ALARM_BMS_OFFLINE;
            BSP_ExitCritical();
            LOG("BMS: OFFLINE (timeout while faulted)\r\n");
        } else {
            g_bms_view.online = true;
            BSP_EnterCritical();
            g_bms_data_stale = is_stale;
            BSP_ExitCritical();
        }

        /* Fault recovery: auto-recover when alarms clear and data resumes */
        BMS_AlarmFlag_t active_alarms = g_bms_view.alarm_flags;
        if (g_bms_state == BMS_STATE_FAULT &&
            (active_alarms == BMS_ALARM_NONE) &&
            (!is_stale)) {
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
    if (g_new_data) {
        uint32_t last_log;
        BSP_EnterCritical();
        last_log = g_last_data_log_tick;
        BSP_ExitCritical();
        if ((now_tick - last_log) >= 1000U) {
            BSP_EnterCritical();
            g_last_data_log_tick = now_tick;
            g_new_data = false;
            BSP_ExitCritical();
            /* Snapshot atomically for logging */
            BMS_View_t snap;
            BSP_EnterCritical();
            snap = *(BMS_View_t *)&g_bms_view;
            BSP_ExitCritical();
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
                "Req=%s%d.%dV/%s%d.%dA fault=0x%08lX warn=0x%08lX\r\n",
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
                 (unsigned long)snap.alarm_flags,
                 (unsigned long)snap.warning_flags);
        }
    }

    g_bms_view.state       = g_bms_state;
    BSP_EnterCritical();
    g_bms_view.last_rx_tick = g_last_valid_rx_tick;
    BSP_ExitCritical();
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

bool BMS_IsDataStale(void)
{
    bool stale;
    BSP_EnterCritical();
    stale = g_bms_data_stale;
    BSP_ExitCritical();
    return stale;
}

bool BMS_HasFreshPrechargeData(uint32_t now_tick)
{
    uint32_t batt_st1_tick;
    uint32_t cell_volt_tick;

    BSP_EnterCritical();
    batt_st1_tick = g_bms_data.last_rx_tick[BMS_FRAME_BATT_ST1];
    cell_volt_tick = g_bms_data.last_rx_tick[BMS_FRAME_CELL_VOLT];
    BSP_ExitCritical();

    if (batt_st1_tick == 0U || cell_volt_tick == 0U) {
        return false;
    }

    return bms_tick_elapsed(now_tick, batt_st1_tick) <= bms_stale_timeout_ms(BMS_FRAME_BATT_ST1) &&
           bms_tick_elapsed(now_tick, cell_volt_tick) <= bms_stale_timeout_ms(BMS_FRAME_CELL_VOLT);
}

void BMS_GetDiagnostics(BMS_Diagnostics_t *diagnostics)
{
    if (diagnostics == NULL) return;

    BSP_EnterCritical();
    memcpy(diagnostics->valid_rx_count, (const void *)g_valid_rx_count,
           sizeof(diagnostics->valid_rx_count));
    diagnostics->unknown_id_count = g_unknown_id_count;
    diagnostics->invalid_dlc_count = g_invalid_dlc_count;
    diagnostics->argument_reject_count = g_argument_reject_count;
    BSP_ExitCritical();
}

bool BMS_HasCriticalAlarm(void)
{
    BMS_AlarmFlag_t crit = bms_critical_alarm_mask();
    return ((g_bms_view.alarm_flags & crit) != 0U);
}

void BMS_GetView(BMS_View_t *view)
{
    if (view == NULL) {
        return;
    }
    BSP_EnterCritical();
    *view = *(BMS_View_t *)&g_bms_view;
    BSP_ExitCritical();
}

bool BMS_ShouldCloseChargeRelay(void)
{
    /* BUGFIX 2026-08-29: this used to also require snap.charge_relay_closed
     * (BmsSwSta's charge_sta bit, the BMS's own self-reported internal
     * charge-relay status) to be true. Confirmed on real hardware: even
     * after ChargeController's update_bms_charge_allow() correctly sends
     * Ctrl_INFO chg_sw=1 (per FR-BMS-06) the whole time RUNNING, this BMS
     * unit's charge_sta bit stayed Open indefinitely -- it does not close
     * its own relay in response to our request in this deployment, so
     * gating our relay on it permanently blocked charging regardless of
     * module/pack voltage. We do not control the BMS's own internal relay
     * in practice; charge_relay_closed stays in BMS_View_t for display/
     * monitoring only (debug_app's Monitor tab), it is not a precondition
     * here anymore. Note: this gate was never actually exercised by any
     * simulator before this fix either -- both test/host_charge_sim/
     * sim_bms.c and the live HIL simulator's bms_relay_allow default to
     * a fixed `true`, independent of what chg_sw we send, so no test had
     * ever proven this bit would follow our request. */
    if (!BMS_IsOnline()) {
        return false;
    }

    if (BMS_HasCriticalAlarm()) {
        return false;
    }

    return true;
}
