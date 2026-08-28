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
/* ISR → main flag: set in FeedFrame, cleared/logged in Process (no LOG in ISR) */
static volatile uint32_t        g_isr_rx_count;
static volatile bool            g_isr_new_data;

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
    /* BUGFIX BUG-09: per bms_protocol.h's own documented ALM_INFO severity
     * scale (0=none, 1=warning, 2=fault, 3=severe), a mere "warning" is not
     * an actionable fault condition -- but this used to treat sev>=1 the
     * same as sev==2/3, so a BMS-side warning on any of the flags in
     * bms_critical_alarm_mask() (e.g. HIGH_CELL_VOLT, TEMP_HIGH_CHG) would
     * trip BMS_STATE_FAULT and refuse the charge relay just like a real
     * fault would. Require fault-or-severe (sev >= 2). */
    if (sev >= 2U) {
        return flag;
    }
    return BMS_ALARM_NONE;
}

static void update_alarm_flags(void)
{
    /* BUGFIX BUG-06: BMS_ALARM_BMS_OFFLINE and BMS_ALARM_STALE_DATA are owned
     * by BMS_Process() (main loop), not by this ISR-context ALM_INFO parse.
     * This used to be a full overwrite of g_bms_view.alarm_flags, so every
     * incoming ALM_INFO frame (every ~100ms per protocol) silently clobbered
     * whichever of those two bits BMS_Process had set -- not a rare timing
     * race, a guaranteed clobber on every frame. Preserve them explicitly. */
    BMS_AlarmFlag_t flags = BMS_ALARM_NONE;
    const volatile BMS_AlmInfo_t *a = &g_bms_data.alm_info;
    const BMS_AlarmFlag_t preserve_mask = (BMS_AlarmFlag_t)(BMS_ALARM_BMS_OFFLINE | BMS_ALARM_STALE_DATA);

    if (a->valid) {
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
    }

    g_bms_view.alarm_flags = (BMS_AlarmFlag_t)(flags | (g_bms_view.alarm_flags & preserve_mask));
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
    g_last_valid_rx_tick = BSP_GetTick();
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
        if (i == BMS_FRAME_BMS_SW_STA || i == BMS_FRAME_CELL_VOLT_FULL || i == BMS_FRAME_CELL_TEMP_FULL) {
            continue; /* Ignore optional/slow frames for stale check */
        }
        uint32_t f_elapsed = bms_tick_elapsed(now_tick, last_rx_frames[i]); /* BUGFIX BUG-02 */
        if (f_elapsed >= BMS_STALE_THRESHOLD_MS) {
            is_stale = true;
            break;
        }
    }

    /* ---- State Machine ---- */
    if (g_bms_state == BMS_STATE_OFFLINE) {
        /* Transition to ONLINE as soon as any valid frame has been parsed;
         * `elapsed` has no bearing on leaving OFFLINE (removed a dead
         * if/else that branched on it without doing anything -- DES-01). */
        if (has_any_valid_bms_data()) {
            g_bms_state = BMS_STATE_ONLINE;
            g_bms_view.online = true;
            LOG("BMS: ONLINE (was OFFLINE, now has data)\r\n");
        }
    }
    else if (g_bms_state == BMS_STATE_ONLINE) {
        if (elapsed >= BMS_OFFLINE_TIMEOUT_MS) {
            g_bms_state = BMS_STATE_OFFLINE;
            /* Clear all parsed data AND the cached view so a stale voltage/
             * SOC/temperature reading is never mistaken for live telemetry
             * while OFFLINE (BUGFIX BUG-03: previously only g_bms_data was
             * cleared -- g_bms_view kept the last-known values). */
            memset((void *)&g_bms_data, 0, sizeof(g_bms_data));
            memset((void *)&g_bms_view, 0, sizeof(g_bms_view));
            g_bms_view.online = false;
            g_bms_view.alarm_flags = BMS_ALARM_BMS_OFFLINE;
            LOG("BMS: OFFLINE (timeout after %lu ms from tick %lu, now %lu)\r\n",
                (unsigned long)elapsed, (unsigned long)last_rx_snapshot, (unsigned long)now_tick);
        } else {
            /* Connectivity is based on any valid BMS frame.  STALE is a
             * data-quality warning, not an offline condition.
             * BUGFIX BUG-04: this only ever set STALE_DATA and never cleared
             * it once data quality recovered, unlike the FAULT branch below
             * (which already has the matching else-clear). Latched forever
             * within a single ONLINE session otherwise. */
            g_bms_view.online = true;
            /* BUGFIX BUG-06: |=/&= is a read-modify-write on a field the ISR
             * (update_alarm_flags) also writes; without this, the ISR could
             * fire between the read and the write and have its update
             * clobbered by this stale snapshot-based store. */
            BSP_EnterCritical();
            if (is_stale) {
                g_bms_view.alarm_flags |= BMS_ALARM_STALE_DATA;
            } else {
                g_bms_view.alarm_flags &= (BMS_AlarmFlag_t)~BMS_ALARM_STALE_DATA;
            }
            BSP_ExitCritical();
            /* Only critical alarms (not STALE_DATA) transition to FAULT */
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
            /* BUGFIX BUG-03: see the matching comment in the ONLINE branch. */
            memset((void *)&g_bms_data, 0, sizeof(g_bms_data));
            memset((void *)&g_bms_view, 0, sizeof(g_bms_view));
            g_bms_view.online = false;
            g_bms_view.alarm_flags = BMS_ALARM_BMS_OFFLINE;
            LOG("BMS: OFFLINE (timeout while faulted)\r\n");
        } else {
            g_bms_view.online = true;
            /* BUGFIX BUG-06: see the matching comment in the ONLINE branch. */
            BSP_EnterCritical();
            if (is_stale) {
                g_bms_view.alarm_flags |= BMS_ALARM_STALE_DATA;
            } else {
                g_bms_view.alarm_flags &= (BMS_AlarmFlag_t)~BMS_ALARM_STALE_DATA;
            }
            BSP_ExitCritical();
        }

        /* Fault recovery: auto-recover when alarms clear and data resumes */
        BMS_AlarmFlag_t active_alarms = g_bms_view.alarm_flags & (BMS_AlarmFlag_t)~BMS_ALARM_STALE_DATA;
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
    if (g_isr_new_data) {
        uint32_t last_log;
        BSP_EnterCritical();
        last_log = g_last_data_log_tick;
        BSP_ExitCritical();
        if ((now_tick - last_log) >= 1000U) {
            BSP_EnterCritical();
            g_last_data_log_tick = now_tick;
            g_isr_new_data = false;
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

bool BMS_HasCriticalAlarm(void)
{
    /* STALE is a connectivity warning, not a critical offline condition. */
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
    if (!BMS_IsOnline()) {
        return false;
    }

    if (BMS_HasCriticalAlarm()) {
        return false;
    }

    BMS_View_t snap;
    BSP_EnterCritical();
    snap = *(BMS_View_t *)&g_bms_view;
    BSP_ExitCritical();

    /* Charge relay in BMS must be closed */
    if (!snap.charge_relay_closed) {
        return false;
    }

    return true;
}





