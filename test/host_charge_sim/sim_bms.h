#ifndef SIM_BMS_H
#define SIM_BMS_H
/**
 * @file sim_bms.h
 * @brief Host-side BMS simulator — builds real BMS_ID_* frames (per
 *        Modules/bms/bms_protocol.c) from a small physical-units state and
 *        feeds them into the firmware via BMS_FeedFrame(), on a schedule
 *        mirroring the real intervals documented in bms_protocol.h.
 */
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    /* BATT_ST1 (20ms) */
    float   pack_voltage_v;
    float   pack_current_a;   /* signed; BMS_RAW_TO_CURR encoding, +/-400A range */
    uint8_t soc_pct;

    /* CELL_VOLT (100ms) */
    uint16_t max_cell_mv;
    uint8_t  max_cv_no;
    uint16_t min_cell_mv;
    uint8_t  min_cv_no;

    /* CELL_TEMP (500ms) -- physical degC, raw = value+50 */
    float max_cell_temp_c;
    uint8_t max_ct_no;
    float min_cell_temp_c;
    uint8_t min_ct_no;
    float avg_cell_temp_c;

    /* ALM_INFO (event-triggered here: sent every cycle) — each field is a
     * 0..3 severity per bms_protocol.h (0=none,1=warning,2=fault,3=severe;
     * severity 1 is reporting-only, severity >=2 is actionable). */
    uint8_t low_pack_volt, low_cell_volt, high_pack_volt, high_cell_volt;
    uint8_t temp_cell_high_chg, temp_cell_high_dchg, temp_cell_low_chg, temp_cell_low_dchg;
    uint8_t temp_relay_high, over_chg_curr, over_dchg_curr, cell_volt_diff, low_soc;

    /* BATT_ST2 (100ms) */
    uint16_t cap_remain_x0_1ah;
    uint16_t rate_cap_x0_1ah;
    uint16_t cycle_count;
    uint8_t  soh_pct;

    /* ChgRequest_INFO (1000ms) */
    float chg_volt_request_v;
    float chg_curr_request_a;

    /* BmsSwSta (500ms) -- byte0 bit0/1/2 per chg_lib's documented mapping
     * (pre-discharge/discharge/charge relay). bms_relay_allow feeds
     * BMS_View_t.charge_relay_closed, which BMS_ShouldCloseChargeRelay()
     * requires before the controller will ever close the battery relay in
     * BMS-Controlled mode -- see App/Charge/charge_controller.c's
     * update_relay_decision(). */
    bool bms_relay_allow;

    /* When false, sim_bms_tick() sends nothing (used to simulate BMS
     * offline / communication loss). */
    bool transmitting;

    /* When false, sim_bms_tick() suppresses 0x07F4 transmission (used to simulate
     * real BMS behavior where ALM_INFO is not sent when clear per §5.4). */
    bool alm_info_tx_enabled;
} SimBmsState_t;

extern SimBmsState_t g_sim_bms;

void sim_bms_reset(SimBmsState_t *b);

/* Send every due frame type for this simulated tick (rate-limited
 * internally to the real intervals). Call once per ~20ms simulated step. */
void sim_bms_tick(uint32_t now_tick);

#endif
