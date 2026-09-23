/**
 * @file    dwin_protocol.c
 * @brief   DWIN DGUS-II HMI protocol (RS485) -- framing + dashboard update
 *          scatter + touch-event parsing. See dwin_vp_map.h for the VP
 *          contract and the wire format.
 *
 * The only platform seam is UART_Transmit_To_DWIN() (provided by BSP,
 * bsp_rs485.c) -- this file has no STM32/HAL dependency and is host-testable.
 */
#include "dwin_protocol.h"
#include <string.h>
#include <stdio.h>

/* Provided by BSP (bsp_rs485.c): drives DE and blocks on HAL_UART_Transmit. */
extern void UART_Transmit_To_DWIN(uint8_t *data, uint16_t len);

/* Largest word count we ever send in one 0x82 frame (alarm description is 32 words).
 * Bounds every stack buffer below. */
#define DWIN_TX_MAX_WORDS  34U

/* RX reassembly bound -- a touch upload is 9 bytes; anything claiming a
 * payload longer than this is noise. */
#define DWIN_RX_MAX_LEN    32U

/* MCU-side synchronization state. These caches must be invalidated when the
 * panel restarts independently of the MCU. */
static int32_t s_last_page = -1;
static uint8_t s_update_step = 0U;
static DWIN_SystemData_t s_prev_data;
static bool s_have_prev_data = false;

/* ===================== TX: low-level framing ===================== */

static void dwin_write_frame(uint16_t vp, const uint8_t *payload, uint8_t payload_len)
{
    /* A5 5A | LEN | 82 | VP_hi VP_lo | payload...   (header is DWIN_HEADER_1/2
     * -- this project uses A5 5A, not the DGUS-stock 5A A5; see dwin_protocol.h)
     * LEN counts cmd(1) + vp(2) + payload_len. */
    uint8_t buf[6U + (2U * DWIN_TX_MAX_WORDS)];

    if (payload_len > (uint8_t)(sizeof(buf) - 6U)) {
        return;
    }

    buf[0] = DWIN_HEADER_1;
    buf[1] = DWIN_HEADER_2;
    buf[2] = (uint8_t)(3U + payload_len);
    buf[3] = DWIN_CMD_WRITE;
    buf[4] = (uint8_t)(vp >> 8);
    buf[5] = (uint8_t)(vp & 0xFFU);
    if (payload_len > 0U) {
        memcpy(&buf[6], payload, payload_len);
    }
    UART_Transmit_To_DWIN(buf, (uint16_t)(6U + payload_len));
}

void DWIN_SendWords(uint16_t vp, const uint16_t *words, uint8_t n_words)
{
    uint8_t payload[2U * DWIN_TX_MAX_WORDS];

    if (words == NULL || n_words == 0U || n_words > DWIN_TX_MAX_WORDS) {
        return;
    }

    for (uint8_t i = 0; i < n_words; i++) {
        payload[2U * i]        = (uint8_t)(words[i] >> 8);
        payload[(2U * i) + 1U] = (uint8_t)(words[i] & 0xFFU);
    }
    dwin_write_frame(vp, payload, (uint8_t)(2U * n_words));
}

void DWIN_SendReadRequest(uint16_t vp, uint8_t n_words)
{
    uint8_t frame[7];
    frame[0] = DWIN_HEADER_1;
    frame[1] = DWIN_HEADER_2;
    frame[2] = 4U;
    frame[3] = DWIN_CMD_READ;
    frame[4] = (uint8_t)(vp >> 8);
    frame[5] = (uint8_t)(vp & 0xFFU);
    frame[6] = n_words;
    UART_Transmit_To_DWIN(frame, 7U);
}

static void dwin_pack_string(uint8_t *dest, const char *src, uint8_t len)
{
    memset(dest, 0, len);
    if (src != NULL) {
        for (uint8_t i = 0; (i < len) && (src[i] != '\0'); i++) {
            dest[i] = (uint8_t)src[i];
        }
    }
}

void DWIN_SendString(uint16_t vp, const char *str, uint8_t field_words)
{
    uint8_t payload[2U * DWIN_TX_MAX_WORDS];
    uint8_t n_bytes;

    if (str == NULL || field_words == 0U || field_words > DWIN_TX_MAX_WORDS) {
        return;
    }

    n_bytes = (uint8_t)(2U * field_words);
    dwin_pack_string(payload, str, n_bytes);
    dwin_write_frame(vp, payload, n_bytes);
}

/* ===================== TX: helpers ===================== */

void DWIN_Init(void)
{
    /* BSP owns the UART; nothing to do here yet. */
}

void DWIN_SetPage(DwinPageId_e page)
{
    uint16_t w[2];

    if ((int32_t)page == s_last_page) {
        return;
    }
    s_last_page = (int32_t)page;

    w[0] = 0x5A01U;              /* D3=0x5A arm, D2=0x01 "page switch" */
    w[1] = (uint16_t)page;
    DWIN_SendWords(VP_SYS_PIC_SET, w, 2);
}

void DWIN_SendSoftwareReset(void)
{
    /* T5L DGUS-II system variable 0x0004. The documented payload is
     * 0x55AA followed by 0x5AA5; the project header remains A5 5A. */
    const uint16_t reset_words[2] = { 0x55AAU, 0x5AA5U };
    DWIN_SendWords(VP_SYS_RESET, reset_words, 2U);
}

void DWIN_Beep(uint8_t duration_x8ms)
{
    uint16_t val = (uint16_t)duration_x8ms;
    DWIN_SendWords(VP_SYS_BUZZER, &val, 1U);
}

void DWIN_SendSettingStrings(const char *hw_ver, const char *fw_ver,
                             const char *device_id)
{
    if (hw_ver != NULL) {
        DWIN_SendString(VP_SET_HW_VER, hw_ver, VP_SET_ID_WORDS);
    }
    if (fw_ver != NULL) {
        DWIN_SendString(VP_SET_FW_VER, fw_ver, VP_SET_ID_WORDS);
    }
    if (device_id != NULL) {
        DWIN_SendString(VP_SET_DEVICE_ID, device_id, VP_SET_ID_WORDS);
    }
}

void DWIN_SetRTC(uint16_t year, uint8_t month, uint8_t day,
                 uint8_t hour, uint8_t minute, uint8_t second)
{
    uint16_t w[4];

    w[0] = 0x5AA5U;
    w[1] = (uint16_t)(((uint16_t)(year % 100U) << 8) | month);
    w[2] = (uint16_t)(((uint16_t)day << 8) | hour);
    w[3] = (uint16_t)(((uint16_t)minute << 8) | second);
    DWIN_SendWords(VP_SYS_RTC_SET, w, 4);
}

/* ===================== Alarm FIFO Ring Buffer ===================== */

typedef struct {
    char     time_str[9];
    char     code_str[9];
    uint16_t desc_utf16[33];
    uint8_t  desc_len;
    bool     valid;
} DwinAlarmRowInternal_t;

static DwinAlarmRowInternal_t s_alarm_rows[VP_ALARM_ROW_COUNT];
static uint16_t s_alarm_dirty = 0U; /* bitmask of rows 0..11 needing update */

void DWIN_Alarm_Push(const char *time_str, const char *code_str,
                     const uint16_t *desc_utf16, uint8_t desc_len)
{
    /* Shift rows 0..2 down to 1..3 */
    for (int8_t i = (int8_t)VP_ALARM_ROW_COUNT - 1; i > 0; i--) {
        s_alarm_rows[i] = s_alarm_rows[i - 1];
    }

    memset(&s_alarm_rows[0], 0, sizeof(s_alarm_rows[0]));
    if (time_str != NULL) {
        strncpy(s_alarm_rows[0].time_str, time_str, sizeof(s_alarm_rows[0].time_str) - 1U);
    }
    if (code_str != NULL) {
        strncpy(s_alarm_rows[0].code_str, code_str, sizeof(s_alarm_rows[0].code_str) - 1U);
    }
    if (desc_utf16 != NULL && desc_len > 0U) {
        uint8_t n = (desc_len > 32U) ? 32U : desc_len;
        memcpy(s_alarm_rows[0].desc_utf16, desc_utf16, n * sizeof(uint16_t));
        s_alarm_rows[0].desc_len = n;
    }
    s_alarm_rows[0].valid = true;

    /* Mark all rows dirty so they emit across subsequent scatter ticks */
    s_alarm_dirty = (1U << VP_ALARM_ROW_COUNT) - 1U;
}

void DWIN_Alarm_ClearAll(void)
{
    memset(s_alarm_rows, 0, sizeof(s_alarm_rows));
    s_alarm_dirty = (1U << VP_ALARM_ROW_COUNT) - 1U;
}

static void dwin_emit_alarm_row(uint8_t row)
{
    if (row >= VP_ALARM_ROW_COUNT) {
        return;
    }
    uint16_t base = VP_ALARM_ROW_BASE + ((uint16_t)row * VP_ALARM_ROW_STRIDE);

    DWIN_SendString(base + ALARM_OFFSET_TIME, s_alarm_rows[row].time_str, 4);
    DWIN_SendString(base + ALARM_OFFSET_CODE, s_alarm_rows[row].code_str, 4);

    uint16_t desc_buf[32];
    memset(desc_buf, 0, sizeof(desc_buf));
    uint8_t n = s_alarm_rows[row].desc_len;
    if (n > 32U) {
        n = 32U;
    }
    for (uint8_t i = 0; i < n; i++) {
        desc_buf[i] = s_alarm_rows[row].desc_utf16[i];
    }
    DWIN_SendWords(base + ALARM_OFFSET_DESC, desc_buf, 32);
}

/* ===================== TX: dashboard scatter ===================== */

enum {
    STEP_DC = 0,         /* text 0x1000..0x100B */
    STEP_BATT_V,         /* text 0x1010..0x101B */
    STEP_AC,             /* text 0x1020..0x102B */
    STEP_TEMP,           /* text 0x1030..0x103B */
    STEP_SOC_STATUS,     /* status icon 0x1041 + SOC text 0x1048 */
    STEP_BTN_MODE,       /* 0x1042 */
    STEP_TOPBAR_FAULT,   /* 0x1044..0x1047 (Text GBK, 4 words) */
    STEP_CHG_DURATION,   /* 0x1050..0x1057 (Text GBK, 8 words) */
    STEP_SETTING_STATS,  /* 0x1118..0x112F (Uptime, Total Ah, Total kWh) */
    STEP_PRECHARGE,      /* 0x1310..0x1319 */
    STEP_ALARM_ROW,      /* Emit 1 alarm row if dirty */
    STEP_COUNT = DWIN_SCATTER_STEP_COUNT
};

static uint8_t s_force_steps = 0;
static bool s_soc_color_valid = false;
static DwinSocColor_e s_soc_color;

static uint16_t dwin_soc_color_to_rgb565(DwinSocColor_e color)
{
    switch (color) {
    case DWIN_SOC_COLOR_CRITICAL: return DWIN_SOC_COLOR_RGB565_CRITICAL;
    case DWIN_SOC_COLOR_LOW:      return DWIN_SOC_COLOR_RGB565_LOW;
    case DWIN_SOC_COLOR_MEDIUM:   return DWIN_SOC_COLOR_RGB565_MEDIUM;
    case DWIN_SOC_COLOR_NORMAL:   return DWIN_SOC_COLOR_RGB565_NORMAL;
    case DWIN_SOC_COLOR_UNAVAILABLE:
    default:                      return DWIN_SOC_COLOR_RGB565_UNAVAILABLE;
    }
}

void DWIN_SetSocColor(DwinSocColor_e color)
{
    uint16_t rgb565;

    if (color > DWIN_SOC_COLOR_NORMAL) {
        color = DWIN_SOC_COLOR_UNAVAILABLE;
    }
    if (s_soc_color_valid && s_soc_color == color) {
        return;
    }

    rgb565 = dwin_soc_color_to_rgb565(color);
    DWIN_SendWords(DWIN_SOC_COLOR_ADDR, &rgb565, 1U);
    s_soc_color = color;
    s_soc_color_valid = true;
}

void DWIN_ForceFullRefresh(void)
{
    s_force_steps = (uint8_t)STEP_COUNT;
    s_alarm_dirty = (1U << VP_ALARM_ROW_COUNT) - 1U;
    s_soc_color_valid = false;
}

void DWIN_InvalidateSyncState(void)
{
    /* A panel reboot loses its page/VP RAM state, while the MCU-side caches
     * still contain the pre-reboot values. Make the next restore replay all
     * state and force the page command even if the page number is unchanged. */
    s_last_page = -1;
    s_update_step = 0U;
    s_have_prev_data = false;
    memset(&s_prev_data, 0, sizeof(s_prev_data));
    DWIN_ForceFullRefresh();
}

void DWIN_UpdateData(const DWIN_SystemData_t *d)
{
    bool first;

    if (d == NULL) {
        return;
    }

    first = !s_have_prev_data || (s_force_steps > 0U);
    if (s_force_steps > 0U) {
        s_force_steps--;
    }

    switch (s_update_step) {
    case STEP_DC: {
        bool dc_changed = first ||
            (strncmp(s_prev_data.dc_voltage_text, d->dc_voltage_text, sizeof(d->dc_voltage_text)) != 0) ||
            (strncmp(s_prev_data.dc_current_text, d->dc_current_text, sizeof(d->dc_current_text)) != 0) ||
            (strncmp(s_prev_data.dc_power_text, d->dc_power_text, sizeof(d->dc_power_text)) != 0);

        if (dc_changed) {
            /* Coalesce VP_DC_VOLTAGE (0x1000), VP_DC_CURRENT (0x1004), VP_DC_POWER (0x1008)
             * into a single 12-word (24-byte) frame. Reduces 3 UART TX calls (with RS485 turnaround)
             * to 1 atomic frame on the wire: A5 5A 1B 82 10 00 [24 bytes]. */
            uint8_t payload[24];
            dwin_pack_string(&payload[0],  d->dc_voltage_text, 8U);
            dwin_pack_string(&payload[8],  d->dc_current_text, 8U);
            dwin_pack_string(&payload[16], d->dc_power_text,   8U);
            dwin_write_frame(VP_DC_VOLTAGE, payload, (uint8_t)sizeof(payload));

            memcpy(s_prev_data.dc_voltage_text, d->dc_voltage_text, sizeof(s_prev_data.dc_voltage_text));
            memcpy(s_prev_data.dc_current_text, d->dc_current_text, sizeof(s_prev_data.dc_current_text));
            memcpy(s_prev_data.dc_power_text,   d->dc_power_text,   sizeof(s_prev_data.dc_power_text));
        }
        break;
    }

    case STEP_BATT_V:
        if (first || strncmp(s_prev_data.bat_pack_volt_text, d->bat_pack_volt_text, sizeof(d->bat_pack_volt_text)) != 0) {
            DWIN_SendString(VP_BAT_PACK_VOLT_TEXT, d->bat_pack_volt_text, DWIN_TEXT_8_BYTES_WORDS);
            memcpy(s_prev_data.bat_pack_volt_text, d->bat_pack_volt_text, sizeof(s_prev_data.bat_pack_volt_text));
        }
        if (first || strncmp(s_prev_data.bat_cell_volt_text, d->bat_cell_volt_text, sizeof(d->bat_cell_volt_text)) != 0) {
            DWIN_SendString(VP_BAT_CELL_VOLT_TEXT, d->bat_cell_volt_text, DWIN_TEXT_8_BYTES_WORDS);
            memcpy(s_prev_data.bat_cell_volt_text, d->bat_cell_volt_text, sizeof(s_prev_data.bat_cell_volt_text));
        }
        if (first || strncmp(s_prev_data.bat_cap_text, d->bat_cap_text, sizeof(d->bat_cap_text)) != 0) {
            DWIN_SendString(VP_BAT_CHARGED_AH_TEXT, d->bat_cap_text, DWIN_TEXT_8_BYTES_WORDS);
            memcpy(s_prev_data.bat_cap_text, d->bat_cap_text, sizeof(s_prev_data.bat_cap_text));
        }
        break;

    case STEP_AC:
        if (first || strncmp(s_prev_data.ac_l1_text, d->ac_l1_text, sizeof(d->ac_l1_text)) != 0) {
            DWIN_SendString(VP_AC_PHASE_L1, d->ac_l1_text, DWIN_TEXT_8_BYTES_WORDS);
            memcpy(s_prev_data.ac_l1_text, d->ac_l1_text, sizeof(s_prev_data.ac_l1_text));
        }
        if (first || strncmp(s_prev_data.ac_l2_text, d->ac_l2_text, sizeof(d->ac_l2_text)) != 0) {
            DWIN_SendString(VP_AC_PHASE_L2, d->ac_l2_text, DWIN_TEXT_8_BYTES_WORDS);
            memcpy(s_prev_data.ac_l2_text, d->ac_l2_text, sizeof(s_prev_data.ac_l2_text));
        }
        if (first || strncmp(s_prev_data.ac_l3_text, d->ac_l3_text, sizeof(d->ac_l3_text)) != 0) {
            DWIN_SendString(VP_AC_PHASE_L3, d->ac_l3_text, DWIN_TEXT_8_BYTES_WORDS);
            memcpy(s_prev_data.ac_l3_text, d->ac_l3_text, sizeof(s_prev_data.ac_l3_text));
        }
        break;

    case STEP_TEMP:
        if (first || strncmp(s_prev_data.temp_battery_text, d->temp_battery_text, sizeof(d->temp_battery_text)) != 0) {
            DWIN_SendString(VP_TEMP_BATTERY_TEXT, d->temp_battery_text, DWIN_TEXT_8_BYTES_WORDS);
            memcpy(s_prev_data.temp_battery_text, d->temp_battery_text, sizeof(s_prev_data.temp_battery_text));
        }
        if (first || strncmp(s_prev_data.temp_charge_text, d->temp_charge_text, sizeof(d->temp_charge_text)) != 0) {
            DWIN_SendString(VP_TEMP_CHARGE_TEXT, d->temp_charge_text, DWIN_TEXT_8_BYTES_WORDS);
            memcpy(s_prev_data.temp_charge_text, d->temp_charge_text, sizeof(s_prev_data.temp_charge_text));
        }
        if (first || strncmp(s_prev_data.temp_jack_text, d->temp_jack_text, sizeof(d->temp_jack_text)) != 0) {
            DWIN_SendString(VP_TEMP_JACK_TEXT, d->temp_jack_text, DWIN_TEXT_8_BYTES_WORDS);
            memcpy(s_prev_data.temp_jack_text, d->temp_jack_text, sizeof(s_prev_data.temp_jack_text));
        }
        break;

    case STEP_SOC_STATUS:
        if (first || s_prev_data.status_icon != d->status_icon) {
            uint16_t status = d->status_icon;
            DWIN_SendWords(VP_SYS_STATUS_ICON, &status, 1);
            s_prev_data.status_icon = d->status_icon;
        }
        if (first || strncmp(s_prev_data.soc_text, d->soc_text, sizeof(d->soc_text)) != 0) {
            DWIN_SendString(DWIN_SOC_TEXT_VP, d->soc_text, DWIN_TEXT_8_BYTES_WORDS);
            memcpy(s_prev_data.soc_text, d->soc_text, sizeof(s_prev_data.soc_text));
        }
        break;

    case STEP_BTN_MODE:
        if (first || s_prev_data.btn_mode != d->btn_mode) {
            uint16_t w = d->btn_mode;
            DWIN_SendWords(VP_SYS_BTN_ICON, &w, 1);
            s_prev_data.btn_mode = d->btn_mode;
        }
        break;

    case STEP_TOPBAR_FAULT:
        if (first || strncmp(s_prev_data.topbar_fault_code, d->topbar_fault_code, sizeof(d->topbar_fault_code)) != 0) {
            DWIN_SendString(VP_TOPBAR_FAULT_CODE, d->topbar_fault_code, 4);
            memcpy(s_prev_data.topbar_fault_code, d->topbar_fault_code, sizeof(s_prev_data.topbar_fault_code));
        }
        break;

    case STEP_CHG_DURATION: {
        char dur_str[16];
        if (d->footer_time_str[0] != '\0') {
            strncpy(dur_str, d->footer_time_str, sizeof(dur_str) - 1U);
            dur_str[sizeof(dur_str) - 1U] = '\0';
        } else {
            uint32_t s = d->charge_duration_s;
            uint32_t h = s / 3600U;
            uint32_t m = (s % 3600U) / 60U;
            uint32_t sec = s % 60U;
            (void)snprintf(dur_str, sizeof(dur_str), "%02u:%02u:%02u",
                           (unsigned)h, (unsigned)m, (unsigned)sec);
        }
        if (first || s_prev_data.charge_duration_s != d->charge_duration_s ||
            strncmp(s_prev_data.footer_time_str, d->footer_time_str, sizeof(d->footer_time_str)) != 0) {
            DWIN_SendString(VP_CHG_DURATION, dur_str, 8);
            s_prev_data.charge_duration_s = d->charge_duration_s;
            memcpy(s_prev_data.footer_time_str, d->footer_time_str, sizeof(s_prev_data.footer_time_str));
        }
        break;
    }

    case STEP_SETTING_STATS:
        if (first || s_prev_data.uptime_s != d->uptime_s ||
            s_prev_data.total_charged_ah_x10 != d->total_charged_ah_x10 ||
            s_prev_data.total_energy_kwh_x10 != d->total_energy_kwh_x10) {
            char str[16];
            (void)snprintf(str, sizeof(str), "%lu.%u Ah",
                           (unsigned long)(d->total_charged_ah_x10 / 10U),
                           (unsigned)(d->total_charged_ah_x10 % 10U));
            DWIN_SendString(VP_SET_TOTAL_CHARGED, str, 8);

            (void)snprintf(str, sizeof(str), "%lu.%u kWh",
                           (unsigned long)(d->total_energy_kwh_x10 / 10U),
                           (unsigned)(d->total_energy_kwh_x10 % 10U));
            DWIN_SendString(VP_SET_TOTAL_ENERGY, str, 8);

            uint32_t u = d->uptime_s;
            uint32_t uh = u / 3600U;
            uint32_t um = (u % 3600U) / 60U;
            uint32_t us = u % 60U;
            (void)snprintf(str, sizeof(str), "%02u:%02u:%02u",
                           (unsigned)uh, (unsigned)um, (unsigned)us);
            DWIN_SendString(VP_SET_UPTIME, str, 8);

            s_prev_data.uptime_s = d->uptime_s;
            s_prev_data.total_charged_ah_x10 = d->total_charged_ah_x10;
            s_prev_data.total_energy_kwh_x10 = d->total_energy_kwh_x10;
        }
        break;

    case STEP_PRECHARGE:
        if (first || strncmp(s_prev_data.precharge_voltage_text, d->precharge_voltage_text,
                             sizeof(d->precharge_voltage_text)) != 0) {
            DWIN_SendString(VP_PRECHARGE_VOLTAGE_TEXT, d->precharge_voltage_text,
                            DWIN_TEXT_8_BYTES_WORDS);
            memcpy(s_prev_data.precharge_voltage_text, d->precharge_voltage_text,
                   sizeof(s_prev_data.precharge_voltage_text));
        }
        if (first || strncmp(s_prev_data.precharge_current_text, d->precharge_current_text,
                             sizeof(d->precharge_current_text)) != 0) {
            DWIN_SendString(VP_PRECHARGE_CURRENT_TEXT, d->precharge_current_text,
                            DWIN_TEXT_8_BYTES_WORDS);
            memcpy(s_prev_data.precharge_current_text, d->precharge_current_text,
                   sizeof(s_prev_data.precharge_current_text));
        }
        if (first || s_prev_data.precharge_status_mode != d->precharge_status_mode) {
            uint16_t status = d->precharge_status_mode;
            DWIN_SendWords(VP_PRECHARGE_STATUS_ICON, &status, 1U);
            s_prev_data.precharge_status_mode = d->precharge_status_mode;
        }
        if (first || s_prev_data.precharge_btn_mode != d->precharge_btn_mode) {
            uint16_t button = d->precharge_btn_mode;
            DWIN_SendWords(VP_PRECHARGE_BTN_ICON, &button, 1U);
            s_prev_data.precharge_btn_mode = d->precharge_btn_mode;
        }
        break;

    case STEP_ALARM_ROW:
        if (s_alarm_dirty != 0U) {
            for (uint8_t r = 0; r < VP_ALARM_ROW_COUNT; r++) {
                if ((s_alarm_dirty & (1U << r)) != 0U) {
                    dwin_emit_alarm_row(r);
                    s_alarm_dirty &= ~(1U << r);
                    break; /* Emit ONE row per scatter cycle */
                }
            }
        }
        break;

    default:
        break;
    }

    s_update_step++;
    if (s_update_step >= STEP_COUNT) {
        s_update_step = 0U;
        s_have_prev_data = true;
    }
}

/* ===================== RX: touch-event parsing ===================== */

/* Weak default: overridden by the composition root. */
#if defined(__GNUC__)
__attribute__((weak))
#endif
void DWIN_OnActionButton(uint16_t keyval)
{
    (void)keyval;
}

#if defined(__GNUC__)
__attribute__((weak))
#endif
void DWIN_OnKeyEvent(uint16_t vp, uint16_t keyval)
{
    (void)vp;
    (void)keyval;
}

void DWIN_ParseRX(const uint8_t *buf, uint16_t len)
{
    static uint8_t  rx[DWIN_RX_MAX_LEN];
    static uint8_t  idx = 0;
    static uint8_t  expected = 0;

    if (buf == NULL) {
        return;
    }

    for (uint16_t i = 0; i < len; i++) {
        uint8_t b = buf[i];

        if (idx == 0U) {
            if (b == DWIN_HEADER_1) {
                rx[idx++] = b;
            }
        } else if (idx == 1U) {
            if (b == DWIN_HEADER_2) {
                rx[idx++] = b;
            } else {
                idx = 0;
                if (b == DWIN_HEADER_1) {
                    rx[idx++] = b;
                }
            }
        } else if (idx == 2U) {
            expected = b;
            rx[idx++] = b;
            if (expected == 0U || (uint16_t)(expected + 3U) > DWIN_RX_MAX_LEN) {
                idx = 0;
            }
        } else {
            rx[idx++] = b;
            if (idx >= (uint8_t)(expected + 3U)) {
                if (rx[3] == DWIN_CMD_READ && expected >= 6U) {
                    uint16_t vp = (uint16_t)(((uint16_t)rx[4] << 8) | rx[5]);
                    uint8_t  nw = rx[6];
                    if (nw >= 1U) {
                        uint16_t keyval =
                            (uint16_t)(((uint16_t)rx[7] << 8) | rx[8]);
                        if (vp == VP_SYS_BTN_KEY && keyval != 0U) {
                            s_last_page = -1;
                            DWIN_OnActionButton(keyval);
                        } else if (vp == VP_CFG_HOURS || vp == VP_CFG_MINUTES) {
                            DWIN_OnKeyEvent(vp, keyval);
                            if (vp == VP_CFG_HOURS && nw >= 2U) {
                                uint16_t minval = (uint16_t)(((uint16_t)rx[9] << 8) | rx[10]);
                                DWIN_OnKeyEvent(VP_CFG_MINUTES, minval);
                            }
                        } else if ((vp == VP_SET_LOGIN_KEY || vp == VP_TIME_MODE_KEY ||
                                    vp == VP_LOGIN_KEY ||
                                    vp == VP_PRECHARGE_ACTION_KEY ||
                                    vp == VP_CFG_APPLY_KEY) && keyval != 0U) {
                            s_last_page = -1;
                            DWIN_OnKeyEvent(vp, keyval);
                        }
                    }
                }
                idx = 0;
            }
        }
    }
}
