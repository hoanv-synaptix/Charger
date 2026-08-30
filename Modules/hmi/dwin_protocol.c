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

/* Provided by BSP (bsp_rs485.c): drives DE and blocks on HAL_UART_Transmit. */
extern void UART_Transmit_To_DWIN(uint8_t *data, uint16_t len);

/* Largest word count we ever send in one 0x82 frame (string fields are the
 * widest at 8 words). Bounds every stack buffer below. */
#define DWIN_TX_MAX_WORDS  8U

/* RX reassembly bound -- a touch upload is 9 bytes; anything claiming a
 * payload longer than this is noise. */
#define DWIN_RX_MAX_LEN    32U

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
        payload[2U * i]      = (uint8_t)(words[i] >> 8);
        payload[(2U * i) + 1U] = (uint8_t)(words[i] & 0xFFU);
    }
    dwin_write_frame(vp, payload, (uint8_t)(2U * n_words));
}

void DWIN_SendString(uint16_t vp, const char *str, uint8_t field_words)
{
    uint8_t payload[2U * DWIN_TX_MAX_WORDS];
    uint8_t n_bytes;

    if (str == NULL || field_words == 0U || field_words > DWIN_TX_MAX_WORDS) {
        return;
    }

    n_bytes = (uint8_t)(2U * field_words);
    memset(payload, 0, n_bytes);
    for (uint8_t i = 0; (i < n_bytes) && (str[i] != '\0'); i++) {
        payload[i] = (uint8_t)str[i];
    }
    dwin_write_frame(vp, payload, n_bytes);
}

/* ===================== TX: helpers ===================== */

void DWIN_Init(void)
{
    /* BSP owns the UART; nothing to do here yet. */
}

void DWIN_SetPage(DwinPageId_e page)
{
    static int32_t last_page = -1;
    uint16_t w[2];

    if ((int32_t)page == last_page) {
        return;
    }
    last_page = (int32_t)page;

    w[0] = 0x5A01U;              /* D3=0x5A arm, D2=0x01 "page switch" */
    w[1] = (uint16_t)page;
    DWIN_SendWords(VP_SYS_PIC_SET, w, 2);
}

void DWIN_SendSettingStrings(const char *hw_ver, const char *fw_ver,
                             const char *device_id)
{
    if (hw_ver != NULL) {
        DWIN_SendString(VP_SET_HW_VER, hw_ver, VP_SET_STR_WORDS);
    }
    if (fw_ver != NULL) {
        DWIN_SendString(VP_SET_FW_VER, fw_ver, VP_SET_STR_WORDS);
    }
    if (device_id != NULL) {
        DWIN_SendString(VP_SET_DEVICE_ID, device_id, VP_SET_STR_WORDS);
    }
}

void DWIN_SetRTC(uint16_t year, uint8_t month, uint8_t day,
                 uint8_t hour, uint8_t minute, uint8_t second)
{
    /* guide sec 5.1, 0x009C: write 0x5AA5 to arm, then 009D=YY:MM,
     * 009E=DD:HH, 009F=MM:SS (packed hi:lo, all HEX). One 4-word write from
     * 0x009C does all of it. */
    uint16_t w[4];

    w[0] = 0x5AA5U;
    w[1] = (uint16_t)(((uint16_t)(year % 100U) << 8) | month);
    w[2] = (uint16_t)(((uint16_t)day << 8) | hour);
    w[3] = (uint16_t)(((uint16_t)minute << 8) | second);
    DWIN_SendWords(VP_SYS_RTC_SET, w, 4);
}

/* ===================== TX: dashboard scatter ===================== */

/* One field-group per DWIN_UpdateData() call. Contiguous VPs are grouped
 * into a single multi-word write. STEP_BTN_MODE writes VP_SYS_BUTTON
 * (0x1043) -- that VP is bidirectional (the panel also uploads the button
 * keycode on press); the write here just drives the label icon, and the RX
 * path re-writes it right after a press (see app_main DWIN_OnActionButton). */
enum {
    STEP_DC = 0,      /* 0x1000..0x1002 */
    STEP_BATT_V,      /* 0x1010..0x1011 */
    STEP_CHARGED_AH,  /* 0x1012..0x1013 (u32) */
    STEP_AC,          /* 0x1020..0x1022 */
    STEP_TEMP,        /* 0x1030..0x1032 (i16) */
    STEP_SOC_STATUS,  /* 0x1040..0x1041 */
    STEP_BTN_MODE,    /* 0x1043 */
    STEP_UPTIME,      /* 0x1118..0x1119 (u32) */
    STEP_COUNT
};

/* >0 => the next this-many DWIN_UpdateData() calls send unconditionally
 * (one full scatter cycle). Only touched from the main loop. */
static uint8_t s_force_steps = 0;

void DWIN_ForceFullRefresh(void)
{
    s_force_steps = (uint8_t)STEP_COUNT;
}

void DWIN_UpdateData(const DWIN_SystemData_t *d)
{
    static uint8_t step = 0;
    static DWIN_SystemData_t prev;
    static bool have_prev = false;
    uint16_t w[3];
    bool first;

    if (d == NULL) {
        return;
    }

    first = !have_prev || (s_force_steps > 0U);
    if (s_force_steps > 0U) {
        s_force_steps--;
    }

    switch (step) {
    case STEP_DC:
        if (first || prev.dc_voltage_x10 != d->dc_voltage_x10 ||
            prev.dc_current_x10 != d->dc_current_x10 ||
            prev.dc_power_w != d->dc_power_w) {
            w[0] = d->dc_voltage_x10;
            w[1] = d->dc_current_x10;
            w[2] = d->dc_power_w;
            DWIN_SendWords(VP_DC_VOLTAGE, w, 3);
        }
        break;

    case STEP_BATT_V:
        if (first || prev.bat_pack_volt_x10 != d->bat_pack_volt_x10 ||
            prev.bat_cell_volt_x100 != d->bat_cell_volt_x100) {
            w[0] = d->bat_pack_volt_x10;
            w[1] = d->bat_cell_volt_x100;
            DWIN_SendWords(VP_BAT_PACK_VOLT, w, 2);
        }
        break;

    case STEP_CHARGED_AH:
        if (first || prev.charged_ah_x10 != d->charged_ah_x10) {
            w[0] = (uint16_t)(d->charged_ah_x10 >> 16);
            w[1] = (uint16_t)(d->charged_ah_x10 & 0xFFFFU);
            DWIN_SendWords(VP_BAT_CHARGED_AH, w, 2);
        }
        break;

    case STEP_AC:
        if (first || prev.ac_l1_v != d->ac_l1_v || prev.ac_l2_v != d->ac_l2_v ||
            prev.ac_l3_v != d->ac_l3_v) {
            w[0] = d->ac_l1_v;
            w[1] = d->ac_l2_v;
            w[2] = d->ac_l3_v;
            DWIN_SendWords(VP_AC_PHASE_L1, w, 3);
        }
        break;

    case STEP_TEMP:
        if (first || prev.temp_battery_c_x10 != d->temp_battery_c_x10 ||
            prev.temp_charge_c_x10 != d->temp_charge_c_x10 ||
            prev.temp_jack_c_x10 != d->temp_jack_c_x10) {
            w[0] = (uint16_t)d->temp_battery_c_x10;
            w[1] = (uint16_t)d->temp_charge_c_x10;
            w[2] = (uint16_t)d->temp_jack_c_x10;
            DWIN_SendWords(VP_TEMP_BATTERY, w, 3);
        }
        break;

    case STEP_SOC_STATUS:
        if (first || prev.soc_pct != d->soc_pct ||
            prev.status_icon != d->status_icon) {
            w[0] = d->soc_pct;
            w[1] = d->status_icon;
            DWIN_SendWords(VP_SOC_VALUE, w, 2);
        }
        break;

    case STEP_BTN_MODE:
        if (first || prev.btn_mode != d->btn_mode) {
            w[0] = d->btn_mode;
            DWIN_SendWords(VP_SYS_BUTTON, w, 1);
        }
        break;

    case STEP_UPTIME:
        if (first || prev.uptime_s != d->uptime_s) {
            w[0] = (uint16_t)(d->uptime_s >> 16);
            w[1] = (uint16_t)(d->uptime_s & 0xFFFFU);
            DWIN_SendWords(VP_SET_UPTIME, w, 2);
        }
        break;

    default:
        break;
    }

    step++;
    if (step >= STEP_COUNT) {
        step = 0;
        prev = *d;
        have_prev = true;
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

void DWIN_ParseRX(const uint8_t *buf, uint16_t len)
{
    /* State persists across calls -- the RS485 RX ISR feeds this one byte
     * (or one chunk) at a time from the ring buffer. */
    static uint8_t  rx[DWIN_RX_MAX_LEN];
    static uint8_t  idx = 0;
    static uint8_t  expected = 0;   /* value of the LEN byte */

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
                /* resync: a stray HEADER_1 starts a fresh frame */
                idx = 0;
                if (b == DWIN_HEADER_1) {
                    rx[idx++] = b;
                }
            }
        } else if (idx == 2U) {
            expected = b;
            rx[idx++] = b;
            /* LEN must fit: header(3) + LEN bytes <= buffer, and a touch
             * upload is LEN=6. Reject anything absurd. */
            if (expected == 0U || (uint16_t)(expected + 3U) > DWIN_RX_MAX_LEN) {
                idx = 0;
            }
        } else {
            rx[idx++] = b;
            if (idx >= (uint8_t)(expected + 3U)) {
                /* full frame: rx[3]=cmd, rx[4..5]=vp, rx[6]=n_words, rx[7..]=data */
                if (rx[3] == DWIN_CMD_READ && expected >= 6U) {
                    uint16_t vp = (uint16_t)(((uint16_t)rx[4] << 8) | rx[5]);
                    uint8_t  nw = rx[6];
                    if (vp == VP_SYS_BUTTON && nw >= 1U) {
                        uint16_t keyval =
                            (uint16_t)(((uint16_t)rx[7] << 8) | rx[8]);
                        /* Any non-zero upload of 0x1043 is a button press
                         * (fixed keycode). The override restores the label
                         * icon on this same VP -- no clear frame here. */
                        if (keyval != 0U) {
                            DWIN_OnActionButton(keyval);
                        }
                    }
                }
                idx = 0;
            }
        }
    }
}
