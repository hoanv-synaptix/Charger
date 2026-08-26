/**
 * @file    bms_protocol.c
 * @brief   BMS CAN Protocol - Parse and Build implementations
 * @note    Protocol: CAN 2.0A/B, 250Kbps, Little-Endian
 *
 * Hardware Interface:
 *   - CAN2: 250Kbps, PB12=RX, PB13=TX
 *   - Extended + Standard frames
 *
 * References:
 *   - "CAN BMS_BB_PKG V1.0.pdf"
 */

#include "bms_protocol.h"
#include <string.h>
#include "stm32g0xx_hal.h"

/* ============== Private: helpers ============== */

static inline uint16_t get_u16_le(const uint8_t *d, uint8_t pos)
{
    return ((uint16_t)d[pos]       ) |
           ((uint16_t)d[pos + 1] << 8);
}

static inline uint16_t get_u16_be(const uint8_t *d, uint8_t pos)
{
    return ((uint16_t)d[pos] << 8) |
           ((uint16_t)d[pos + 1]     );
}

static inline uint32_t get_u32_le(const uint8_t *d, uint8_t pos)
{
    return ((uint32_t)d[pos]       ) |
           ((uint32_t)d[pos + 1] << 8)  |
           ((uint32_t)d[pos + 2] << 16) |
           ((uint32_t)d[pos + 3] << 24);
}

/* ============== Private: parse individual messages ============== */

/* ---- BATT_ST1: ID=0x02F4, 20ms ---- */
static void parse_batt_st1(const uint8_t *d, BMS_Data_t *bms, uint32_t ext_id)
{
    BMS_BattSt1_t *out = &bms->batt_st1;
    out->raw_volt = get_u16_le(d, 0);
    out->raw_curr = (int16_t)get_u16_le(d, 2);
    out->soc      = d[4];
    out->valid    = true;
}

/* ---- CELL_VOLT: ID=0x04F4, 100ms ---- */
static void parse_cell_volt(const uint8_t *d, BMS_Data_t *bms, uint32_t ext_id)
{
    BMS_CellVolt_t *out = &bms->cell_volt;
    out->max_cell_volt = get_u16_le(d, 0);
    out->max_cv_no     = d[2];
    out->min_cell_volt = get_u16_le(d, 3);
    out->min_cv_no     = d[5];
    out->valid         = true;
}

/* ---- CELL_TEMP: ID=0x05F4, 500ms ---- */
static void parse_cell_temp(const uint8_t *d, BMS_Data_t *bms, uint32_t ext_id)
{
    BMS_CellTemp_t *out = &bms->cell_temp;
    out->max_cell_temp = d[0];
    out->max_ct_no     = d[1];
    out->min_cell_temp = d[2];
    out->min_ct_no     = d[3];
    out->avg_cell_temp = d[4];
    out->valid         = true;
}

/* ---- ALM_INFO: ID=0x07F4, event-triggered ---- */
static void parse_alm_info(const uint8_t *d, BMS_Data_t *bms, uint32_t ext_id)
{
    BMS_AlmInfo_t *out = &bms->alm_info;
    uint32_t raw = get_u32_le(d, 0);  /* Only 26 bits used (4 bytes), little-endian */

    out->low_pack_volt      = (uint8_t)((raw >> 0)  & 0x03U);
    out->low_cell_volt      = (uint8_t)((raw >> 2)  & 0x03U);
    out->high_pack_volt     = (uint8_t)((raw >> 4)  & 0x03U);
    out->high_cell_volt     = (uint8_t)((raw >> 6)  & 0x03U);
    out->temp_cell_high_chg = (uint8_t)((raw >> 8)  & 0x03U);
    out->temp_cell_high_dchg= (uint8_t)((raw >> 10) & 0x03U);
    out->temp_cell_low_chg  = (uint8_t)((raw >> 12) & 0x03U);
    out->temp_cell_low_dchg = (uint8_t)((raw >> 14) & 0x03U);
    out->temp_relay_high    = (uint8_t)((raw >> 16) & 0x03U);
    out->over_chg_curr      = (uint8_t)((raw >> 18) & 0x03U);
    out->over_dchg_curr     = (uint8_t)((raw >> 20) & 0x03U);
    out->cell_volt_diff     = (uint8_t)((raw >> 22) & 0x03U);
    out->low_soc            = (uint8_t)((raw >> 24) & 0x03U);
    out->valid = true;
}

/* ---- BATT_ST2: ID=0x18F128F4, 100ms ---- */
static void parse_batt_st2(const uint8_t *d, BMS_Data_t *bms, uint32_t ext_id)
{
    BMS_BattSt2_t *out = &bms->batt_st2;
    out->cap_remain   = get_u16_le(d, 0);
    out->rate_cap     = get_u16_le(d, 2);
    out->cycle_count  = get_u16_le(d, 4);
    out->soh          = d[6];
    out->valid        = true;
}

/* ---- ChgRequest_INFO: ID=0x1806E5F4, 1000ms ---- */
static void parse_chg_request(const uint8_t *d, BMS_Data_t *bms, uint32_t ext_id)
{
    BMS_ChgRequest_t *out = &bms->chg_request;
    out->batt_volt_req = get_u16_be(d, 0);
    out->batt_curr_req = get_u16_be(d, 2);
    out->valid         = true;
}

/* ---- BmsSwSta: ID=0x18F528F4, 500ms ---- */
static void parse_bms_sw_sta(const uint8_t *d, BMS_Data_t *bms, uint32_t ext_id)
{
    BMS_BmsSwSta_t *out = &bms->bms_sw_sta;
    /* Vendor PDF lists all three start positions as 0. Treat that as a table
     * error and map three independent 1-bit fields in byte0.
     */
    uint8_t byte0 = d[0];
    out->pre_discharge_sta = ((byte0 & 0x01U) != 0U);
    out->discharge_sta    = ((byte0 & 0x02U) != 0U);
    out->charge_sta        = ((byte0 & 0x04U) != 0U);
    out->valid             = true;
}

/* ---- CELL_VOLT_FULL: ID=0x18E028F4..0x18E728F4, 1000ms ---- */
static void parse_cell_volt_full(const uint8_t *d, BMS_Data_t *bms, uint32_t ext_id)
{
    uint8_t frame_idx = (uint8_t)((ext_id >> 16U) & 0x07U);
    BMS_CellVoltFull_t *out = bms->cell_volt_full;
    if (frame_idx >= BMS_MAX_CELL_VOLT_FRAMES) {
        return;
    }

    /* Each frame contains 4 cells, 16 bits each (little-endian) */
    for (uint8_t i = 0U; i < 4U; i++) {
        out[frame_idx].cell[i] = get_u16_le(d, (uint8_t)(i * 2U));
    }
    out[frame_idx].valid = true;
}

/* ---- CELL_TEMP_FULL: ID=0x18F228F4, 1000ms ---- */
static void parse_cell_temp_full(const uint8_t *d, BMS_Data_t *bms, uint32_t ext_id)
{
    BMS_CellTempFull_t *out = &bms->cell_temp_full;
    out->temp_relay        = d[0];
    out->temp_shunt        = d[1];
    out->cell_temp[0]     = d[2];
    out->cell_temp[1]     = d[3];
    out->cell_temp[2]     = d[4];
    out->cell_temp[3]     = d[5];
    out->cell_temp[4]     = d[6];
    out->cell_temp[5]     = d[7];
    out->valid            = true;
}

/* ============== Public: dispatcher ============== */

typedef void (*BMS_ParseFunc_t)(const uint8_t *d, BMS_Data_t *bms, uint32_t ext_id);

typedef struct {
    bool is_ext;
    uint32_t id;
    uint32_t id_mask;
    uint8_t min_dlc;
    BMS_FrameType_t type;
    BMS_ParseFunc_t func;
} BMS_FrameHandler_t;

static const BMS_FrameHandler_t g_handlers[] = {
    {false, BMS_ID_BATT_ST1,       0xFFFFFFFF, 5, BMS_FRAME_BATT_ST1,       parse_batt_st1},
    {false, BMS_ID_CELL_VOLT,      0xFFFFFFFF, 6, BMS_FRAME_CELL_VOLT,      parse_cell_volt},
    {false, BMS_ID_CELL_TEMP,      0xFFFFFFFF, 5, BMS_FRAME_CELL_TEMP,      parse_cell_temp},
    {false, BMS_ID_ALM_INFO,       0xFFFFFFFF, 4, BMS_FRAME_ALM_INFO,       parse_alm_info},
    {true,  BMS_ID_BATT_ST2,       0xFFFFFFFF, 7, BMS_FRAME_BATT_ST2,       parse_batt_st2},
    {true,  BMS_ID_CHG_REQUEST,    0xFFFFFFFF, 4, BMS_FRAME_CHG_REQUEST,    parse_chg_request},
    {true,  BMS_ID_CHG_REQUEST_ALT_CANDIDATE, 0xFFFFFFFF, 4, BMS_FRAME_CHG_REQUEST, parse_chg_request},
    {true,  BMS_ID_BMS_SW_STA,     0xFFFFFFFF, 1, BMS_FRAME_BMS_SW_STA,     parse_bms_sw_sta},
    {true,  BMS_ID_CELL_TEMP_FULL, 0xFFFFFFFF, 8, BMS_FRAME_CELL_TEMP_FULL, parse_cell_temp_full},
    {true,  BMS_ID_CELL_VOLT_FULL(0), BMS_ID_CELL_VOLT_FULL_MASK, 8, BMS_FRAME_CELL_VOLT_FULL, parse_cell_volt_full},
};

bool BMS_ParseFrame(uint32_t ext_id, uint32_t std_id,
                    const uint8_t *data, uint8_t dlc,
                    BMS_Data_t *bms)
{
    if (data == NULL || bms == NULL || dlc > 8U) {
        return false;
    }

    for (size_t i = 0; i < sizeof(g_handlers)/sizeof(g_handlers[0]); i++) {
        const BMS_FrameHandler_t *h = &g_handlers[i];
        if (h->is_ext) {
            if (std_id == 0 && (ext_id & h->id_mask) == h->id) {
                if (dlc >= h->min_dlc) {
                    h->func(data, bms, ext_id);
                    bms->last_rx_tick[h->type] = HAL_GetTick();
                    return true;
                }
            }
        } else {
            if (std_id != 0 && (std_id & h->id_mask) == h->id) {
                if (dlc >= h->min_dlc) {
                    h->func(data, bms, std_id);
                    bms->last_rx_tick[h->type] = HAL_GetTick();
                    return true;
                }
            }
        }
    }

    return false;
}

/* ============== Public: build Ctrl_INFO ============== */

void BMS_BuildCtrlInfo(uint8_t out[8], const BMS_CtrlInfo_t *ctrl)
{
    memset(out, 0, 8U);
    out[0] = ctrl->mask_code;
    out[1] = ctrl->chg_sw;
    out[2] = ctrl->dchg_sw;
    /* bytes 3-7 remain 0 (reserved) */
}

