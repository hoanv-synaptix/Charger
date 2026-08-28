/**
 * @file sim_bms.c
 * @brief Host-side BMS simulator — see sim_bms.h. Frame IDs/byte layouts
 *        copied from Modules/bms/bms_protocol.c (the ground truth for what
 *        the firmware actually parses).
 */
#include "sim_bms.h"
#include "bms_core.h"
#include <string.h>

SimBmsState_t g_sim_bms;

/* Per-frame-type "last sent" tick, used to rate-limit sim_bms_tick() to the
 * real intervals. File-scope (not function-local) so sim_bms_reset() can
 * zero them together with the rest of the simulated state -- otherwise a
 * later scenario's mock_tick (which restarts near 0) would look "not due
 * yet" relative to a leftover large value from an earlier scenario. */
static uint32_t s_last_st1, s_last_cell_volt, s_last_cell_temp;
static uint32_t s_last_alm, s_last_st2, s_last_chg_req, s_last_sw_sta;

void sim_bms_reset(SimBmsState_t *b)
{
    memset(b, 0, sizeof(*b));
    b->transmitting = true;
    b->soh_pct = 100;
    b->bms_relay_allow = true; /* healthy-BMS default; scenarios flip it explicitly */
    s_last_st1 = s_last_cell_volt = s_last_cell_temp = 0;
    s_last_alm = s_last_st2 = s_last_chg_req = s_last_sw_sta = 0;
}

static void put_u16_le(uint8_t *d, uint16_t v) { d[0] = (uint8_t)v; d[1] = (uint8_t)(v >> 8); }
static void put_u16_be(uint8_t *d, uint16_t v) { d[0] = (uint8_t)(v >> 8); d[1] = (uint8_t)v; }

static void send_batt_st1(void)
{
    uint16_t raw_volt = (uint16_t)(g_sim_bms.pack_voltage_v * 10.0f);
    int16_t  raw_curr = (int16_t)((g_sim_bms.pack_current_a + 400.0f) * 10.0f);
    uint8_t d[8] = {0};
    put_u16_le(&d[0], raw_volt);
    put_u16_le(&d[2], (uint16_t)raw_curr);
    d[4] = g_sim_bms.soc_pct;
    BMS_FeedFrame(0, 0x02F4U, d, 8);
}

static void send_cell_volt(void)
{
    uint8_t d[8] = {0};
    put_u16_le(&d[0], g_sim_bms.max_cell_mv);
    d[2] = g_sim_bms.max_cv_no;
    put_u16_le(&d[3], g_sim_bms.min_cell_mv);
    d[5] = g_sim_bms.min_cv_no;
    BMS_FeedFrame(0, 0x04F4U, d, 8);
}

static void send_cell_temp(void)
{
    uint8_t d[8] = {0};
    d[0] = (uint8_t)(g_sim_bms.max_cell_temp_c + 50.0f);
    d[1] = g_sim_bms.max_ct_no;
    d[2] = (uint8_t)(g_sim_bms.min_cell_temp_c + 50.0f);
    d[3] = g_sim_bms.min_ct_no;
    d[4] = (uint8_t)(g_sim_bms.avg_cell_temp_c + 50.0f);
    BMS_FeedFrame(0, 0x05F4U, d, 8);
}

static void send_alm_info(void)
{
    uint32_t raw = 0;
    raw |= ((uint32_t)g_sim_bms.low_pack_volt & 0x03U) << 0;
    raw |= ((uint32_t)g_sim_bms.low_cell_volt & 0x03U) << 2;
    raw |= ((uint32_t)g_sim_bms.high_pack_volt & 0x03U) << 4;
    raw |= ((uint32_t)g_sim_bms.high_cell_volt & 0x03U) << 6;
    raw |= ((uint32_t)g_sim_bms.temp_cell_high_chg & 0x03U) << 8;
    raw |= ((uint32_t)g_sim_bms.temp_cell_high_dchg & 0x03U) << 10;
    raw |= ((uint32_t)g_sim_bms.temp_cell_low_chg & 0x03U) << 12;
    raw |= ((uint32_t)g_sim_bms.temp_cell_low_dchg & 0x03U) << 14;
    raw |= ((uint32_t)g_sim_bms.temp_relay_high & 0x03U) << 16;
    raw |= ((uint32_t)g_sim_bms.over_chg_curr & 0x03U) << 18;
    raw |= ((uint32_t)g_sim_bms.over_dchg_curr & 0x03U) << 20;
    raw |= ((uint32_t)g_sim_bms.cell_volt_diff & 0x03U) << 22;
    raw |= ((uint32_t)g_sim_bms.low_soc & 0x03U) << 24;

    uint8_t d[8] = {0};
    d[0] = (uint8_t)(raw >> 0);
    d[1] = (uint8_t)(raw >> 8);
    d[2] = (uint8_t)(raw >> 16);
    d[3] = (uint8_t)(raw >> 24);
    BMS_FeedFrame(0, 0x07F4U, d, 8);
}

static void send_batt_st2(void)
{
    uint8_t d[8] = {0};
    put_u16_le(&d[0], g_sim_bms.cap_remain_x0_1ah);
    put_u16_le(&d[2], g_sim_bms.rate_cap_x0_1ah);
    put_u16_le(&d[4], g_sim_bms.cycle_count);
    d[6] = g_sim_bms.soh_pct;
    BMS_FeedFrame(0x18F128F4UL, 0, d, 8);
}

static void send_chg_request(void)
{
    uint16_t volt_req = (uint16_t)(g_sim_bms.chg_volt_request_v * 10.0f);
    uint16_t curr_req = (uint16_t)(g_sim_bms.chg_curr_request_a * 10.0f);
    uint8_t d[8] = {0};
    put_u16_be(&d[0], volt_req);
    put_u16_be(&d[2], curr_req);
    BMS_FeedFrame(0x1806E5F4UL, 0, d, 8);
}

/* BmsSwSta byte0: bit0=pre-discharge, bit1=discharge, bit2=charge relay
 * (chg_lib's documented mapping -- vendor PDF lists all three at the same
 * start position, treated as an error; see bms_protocol.c's parse_bms_sw_sta). */
static void send_bms_sw_sta(void)
{
    uint8_t d[8] = {0};
    if (g_sim_bms.bms_relay_allow) {
        d[0] |= (1U << 2);
    }
    BMS_FeedFrame(0x18F528F4UL, 0, d, 8);
}

void sim_bms_tick(uint32_t now_tick)
{
    if (!g_sim_bms.transmitting) return;

    if (now_tick - s_last_st1 >= 20U)   { send_batt_st1();    s_last_st1 = now_tick; }
    if (now_tick - s_last_cell_volt >= 100U) { send_cell_volt();   s_last_cell_volt = now_tick; }
    if (now_tick - s_last_cell_temp >= 500U) { send_cell_temp();   s_last_cell_temp = now_tick; }
    if (now_tick - s_last_alm >= 500U)  { send_alm_info();    s_last_alm = now_tick; }
    if (now_tick - s_last_st2 >= 100U)  { send_batt_st2();    s_last_st2 = now_tick; }
    if (now_tick - s_last_chg_req >= 1000U) { send_chg_request(); s_last_chg_req = now_tick; }
    if (now_tick - s_last_sw_sta >= 500U) { send_bms_sw_sta(); s_last_sw_sta = now_tick; }
}
