/**
 * @file    dwin_alarm_text.c
 * @brief   Vietnamese Unicode (UTF-16) alarm descriptions and code mappings.
 */
#include "dwin_alarm_text.h"
#include <stddef.h>

typedef struct {
    AlarmCode_t     code;
    const char     *code_str;
    const uint16_t *desc_utf16;
} DwinAlarmEntry_t;

/* Standard Vietnamese technical translations for all system alarms.
 * u"..." generates 16-bit Unicode codepoints in GCC. */
static const DwinAlarmEntry_t s_alarm_entries[ALARM_CODE_COUNT] = {
    [ALARM_NONE] = {
        ALARM_NONE,
        "0000",
        (const uint16_t *)u"Hệ thống bình thường"
    },
    [ALARM_BMS_LOW_PACK_VOLT] = {
        ALARM_BMS_LOW_PACK_VOLT,
        "E001",
        (const uint16_t *)u"Điện áp pack BMS thấp"
    },
    [ALARM_BMS_LOW_CELL_VOLT] = {
        ALARM_BMS_LOW_CELL_VOLT,
        "E002",
        (const uint16_t *)u"Điện áp cell BMS thấp"
    },
    [ALARM_BMS_HIGH_PACK_VOLT] = {
        ALARM_BMS_HIGH_PACK_VOLT,
        "E003",
        (const uint16_t *)u"Điện áp pack BMS cao"
    },
    [ALARM_BMS_HIGH_CELL_VOLT] = {
        ALARM_BMS_HIGH_CELL_VOLT,
        "E004",
        (const uint16_t *)u"Quá áp cell pin BMS"
    },
    [ALARM_BMS_TEMP_HIGH_CHG] = {
        ALARM_BMS_TEMP_HIGH_CHG,
        "E005",
        (const uint16_t *)u"Quá nhiệt khi sạc BMS"
    },
    [ALARM_BMS_TEMP_HIGH_DCHG] = {
        ALARM_BMS_TEMP_HIGH_DCHG,
        "W001",
        (const uint16_t *)u"Quá nhiệt khi xả BMS"
    },
    [ALARM_BMS_TEMP_LOW_CHG] = {
        ALARM_BMS_TEMP_LOW_CHG,
        "E006",
        (const uint16_t *)u"Nhiệt độ sạc BMS thấp"
    },
    [ALARM_BMS_TEMP_LOW_DCHG] = {
        ALARM_BMS_TEMP_LOW_DCHG,
        "W002",
        (const uint16_t *)u"Nhiệt độ xả BMS thấp"
    },
    [ALARM_BMS_TEMP_RELAY_HIGH] = {
        ALARM_BMS_TEMP_RELAY_HIGH,
        "W003",
        (const uint16_t *)u"Quá nhiệt rơ le BMS"
    },
    [ALARM_BMS_OVER_CHG_CURR] = {
        ALARM_BMS_OVER_CHG_CURR,
        "E007",
        (const uint16_t *)u"Quá dòng sạc BMS"
    },
    [ALARM_BMS_OVER_DCHG_CURR] = {
        ALARM_BMS_OVER_DCHG_CURR,
        "W004",
        (const uint16_t *)u"Quá dòng xả BMS"
    },
    [ALARM_BMS_CELL_VOLT_DIFF] = {
        ALARM_BMS_CELL_VOLT_DIFF,
        "W005",
        (const uint16_t *)u"Lệch điện áp cell BMS"
    },
    [ALARM_BMS_LOW_SOC] = {
        ALARM_BMS_LOW_SOC,
        "W006",
        (const uint16_t *)u"Mức pin BMS thấp"
    },
    [ALARM_MOD_HW_FAULT] = {
        ALARM_MOD_HW_FAULT,
        "E010",
        (const uint16_t *)u"Lỗi phần cứng module sạc"
    },
    [ALARM_MOD_COMM_FAIL] = {
        ALARM_MOD_COMM_FAIL,
        "W010",
        (const uint16_t *)u"Mất giao tiếp module sạc"
    },
    [ALARM_MOD_OVER_TEMP] = {
        ALARM_MOD_OVER_TEMP,
        "E011",
        (const uint16_t *)u"Quá nhiệt module sạc"
    },
    [ALARM_MOD_OVER_VOLT_OUT] = {
        ALARM_MOD_OVER_VOLT_OUT,
        "E012",
        (const uint16_t *)u"Quá áp đầu ra module sạc"
    },
    [ALARM_MOD_SHORT_CIRCUIT] = {
        ALARM_MOD_SHORT_CIRCUIT,
        "E013",
        (const uint16_t *)u"Ngắn mạch đầu ra module"
    },
    [ALARM_MOD_AC_UNDER_VOLT] = {
        ALARM_MOD_AC_UNDER_VOLT,
        "W011",
        (const uint16_t *)u"Điện áp AC vào module thấp"
    },
    [ALARM_MOD_OVER_CURR_OUT] = {
        ALARM_MOD_OVER_CURR_OUT,
        "E014",
        (const uint16_t *)u"Quá dòng đầu ra module sạc"
    },
    [ALARM_MOD_PFC_FAULT] = {
        ALARM_MOD_PFC_FAULT,
        "E015",
        (const uint16_t *)u"Lỗi mạch PFC module sạc"
    },
    [ALARM_CTRL_NO_MODULE] = {
        ALARM_CTRL_NO_MODULE,
        "E027",
        (const uint16_t *)u"Không tìm thấy module sạc"
    },
    [ALARM_CTRL_MODULE_MISMATCH] = {
        ALARM_CTRL_MODULE_MISMATCH,
        "E028",
        (const uint16_t *)u"Số lượng module không khớp"
    },
    [ALARM_CTRL_INVALID_CONFIG] = {
        ALARM_CTRL_INVALID_CONFIG,
        "E029",
        (const uint16_t *)u"Cấu hình sạc không hợp lệ"
    },
    [ALARM_CTRL_JACK_OVER_V] = {
        ALARM_CTRL_JACK_OVER_V,
        "E030",
        (const uint16_t *)u"Bảo vệ quá áp jack cắm"
    },
    [ALARM_CTRL_JACK_OVER_TEMP] = {
        ALARM_CTRL_JACK_OVER_TEMP,
        "E031",
        (const uint16_t *)u"Bảo vệ quá nhiệt jack cắm"
    },
    [ALARM_CTRL_EMERGENCY_STOP] = {
        ALARM_CTRL_EMERGENCY_STOP,
        "E020",
        (const uint16_t *)u"Dừng khẩn cấp (E-Stop)"
    },
    [ALARM_BMS_COMM_LOST] = {
        ALARM_BMS_COMM_LOST,
        "E021",
        (const uint16_t *)u"Mất kết nối BMS khi sạc"
    },
    [ALARM_BMS_NO_PACK_VOLTAGE] = {
        ALARM_BMS_NO_PACK_VOLTAGE,
        "E022",
        (const uint16_t *)u"Không có điện áp pin"
    },
    [ALARM_DC_LOAD_LOST] = {
        ALARM_DC_LOAD_LOST,
        "E023",
        (const uint16_t *)u"Mất tải đầu ra DC"
    },
    [ALARM_DC_OUT_NOT_ESTABLISHED] = {
        ALARM_DC_OUT_NOT_ESTABLISHED,
        "E024",
        (const uint16_t *)u"Chưa thiết lập đầu ra DC"
    },
    [ALARM_AC_PHASE_LOSS] = {
        ALARM_AC_PHASE_LOSS,
        "E025",
        (const uint16_t *)u"Mất pha điện áp AC vào"
    },
    [ALARM_AC_UNDERVOLT] = {
        ALARM_AC_UNDERVOLT,
        "E026",
        (const uint16_t *)u"Sụt áp nguồn AC đầu vào"
    },
    [ALARM_MOD_FAN_FAULT] = {
        ALARM_MOD_FAN_FAULT,
        "E016",
        (const uint16_t *)u"L\u1ed7i qu\u1ea1t module s\u1ea1c"
    },
    [ALARM_MOD_AC_OVER_VOLT] = {
        ALARM_MOD_AC_OVER_VOLT,
        "E017",
        (const uint16_t *)u"Qu\u00e1 \u00e1p AC \u0111\u1ea7u v\u00e0o module"
    }
};

const char* DWIN_Alarm_GetCodeString(AlarmCode_t code)
{
    if (code >= ALARM_CODE_COUNT) {
        return "E999";
    }
    return s_alarm_entries[code].code_str;
}

const uint16_t* DWIN_Alarm_GetDescUtf16(AlarmCode_t code, uint8_t *out_len)
{
    const uint16_t *str;
    if (code >= ALARM_CODE_COUNT) {
        str = (const uint16_t *)u"Lỗi không xác định";
    } else {
        str = s_alarm_entries[code].desc_utf16;
    }

    if (out_len != NULL) {
        uint8_t len = 0;
        while (str[len] != 0U && len < 32U) {
            len++;
        }
        *out_len = len;
    }
    return str;
}
