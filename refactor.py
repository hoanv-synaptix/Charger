import re

with open('Modules/bms/bms_protocol.c', 'r') as f:
    text = f.read()

text = text.replace('static void parse_batt_st1(const uint8_t *d, BMS_BattSt1_t *out)', 'static void parse_batt_st1(const uint8_t *d, BMS_Data_t *bms, uint32_t ext_id)\n{\n    BMS_BattSt1_t *out = &bms->batt_st1;')
text = text.replace('static void parse_cell_volt(const uint8_t *d, BMS_CellVolt_t *out)', 'static void parse_cell_volt(const uint8_t *d, BMS_Data_t *bms, uint32_t ext_id)\n{\n    BMS_CellVolt_t *out = &bms->cell_volt;')
text = text.replace('static void parse_cell_temp(const uint8_t *d, BMS_CellTemp_t *out)', 'static void parse_cell_temp(const uint8_t *d, BMS_Data_t *bms, uint32_t ext_id)\n{\n    BMS_CellTemp_t *out = &bms->cell_temp;')
text = text.replace('static void parse_alm_info(const uint8_t *d, BMS_AlmInfo_t *out)', 'static void parse_alm_info(const uint8_t *d, BMS_Data_t *bms, uint32_t ext_id)\n{\n    BMS_AlmInfo_t *out = &bms->alm_info;')
text = text.replace('static void parse_batt_st2(const uint8_t *d, BMS_BattSt2_t *out)', 'static void parse_batt_st2(const uint8_t *d, BMS_Data_t *bms, uint32_t ext_id)\n{\n    BMS_BattSt2_t *out = &bms->batt_st2;')
text = text.replace('static void parse_chg_request(const uint8_t *d, BMS_ChgRequest_t *out)', 'static void parse_chg_request(const uint8_t *d, BMS_Data_t *bms, uint32_t ext_id)\n{\n    BMS_ChgRequest_t *out = &bms->chg_request;')
text = text.replace('static void parse_bms_sw_sta(const uint8_t *d, BMS_BmsSwSta_t *out)', 'static void parse_bms_sw_sta(const uint8_t *d, BMS_Data_t *bms, uint32_t ext_id)\n{\n    BMS_BmsSwSta_t *out = &bms->bms_sw_sta;')
text = text.replace('static void parse_cell_volt_full(const uint8_t *d, uint8_t frame_idx,\n                                BMS_CellVoltFull_t *out)', 'static void parse_cell_volt_full(const uint8_t *d, BMS_Data_t *bms, uint32_t ext_id)\n{\n    uint8_t frame_idx = (uint8_t)((ext_id >> 16U) & 0x07U);\n    BMS_CellVoltFull_t *out = bms->cell_volt_full;')
text = text.replace('static void parse_cell_temp_full(const uint8_t *d, BMS_CellTempFull_t *out)', 'static void parse_cell_temp_full(const uint8_t *d, BMS_Data_t *bms, uint32_t ext_id)\n{\n    BMS_CellTempFull_t *out = &bms->cell_temp_full;')

# Now replace the dispatcher
new_dispatcher = """
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

BMS_FrameType_t BMS_ParseFrame(uint32_t ext_id, uint32_t std_id, const uint8_t *data, uint8_t dlc, BMS_Data_t *bms)
{
    if (data == NULL || bms == NULL || dlc > 8U) return BMS_FRAME_MAX;

    for (size_t i = 0; i < sizeof(g_handlers)/sizeof(g_handlers[0]); i++) {
        const BMS_FrameHandler_t *h = &g_handlers[i];
        if (h->is_ext) {
            if (std_id == 0 && (ext_id & h->id_mask) == h->id) {
                if (dlc >= h->min_dlc) {
                    h->func(data, bms, ext_id);
                    return h->type;
                }
            }
        } else {
            if (std_id != 0 && (std_id & h->id_mask) == h->id) {
                if (dlc >= h->min_dlc) {
                    h->func(data, bms, ext_id);
                    return h->type;
                }
            }
        }
    }
    return BMS_FRAME_MAX;
}
"""

text = re.sub(r'bool BMS_ParseFrame\(.*?\n\{.*?\n\}', new_dispatcher, text, flags=re.DOTALL)

with open('Modules/bms/bms_protocol.c', 'w') as f:
    f.write(text)

