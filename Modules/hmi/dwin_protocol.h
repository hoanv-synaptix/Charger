#ifndef DWIN_PROTOCOL_H
#define DWIN_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

#include "dwin_vp_map.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Frame header, first byte then second byte, on both TX and RX.
 *
 * NOTE: the T5L_DGUSII guide V2.9 sec 4.2 specifies 0x5A 0xA5 and every
 * example uses that order; a stock DGUS panel scans for exactly that byte
 * sequence. This build is set to 0xA5 0x5A at the user's request
 * (2026-08-30). */
#define DWIN_HEADER_1   0xA5U
#define DWIN_HEADER_2   0x5AU
#define DWIN_CMD_WRITE  0x82U
#define DWIN_CMD_READ   0x83U

/**
 * @brief Snapshot of everything shown on the dashboard + setting screens.
 * @note  Built by the composition root (App/System/app_main.c) each HMI tick
 *        from the charge-controller / BMS / charger views. This module holds
 *        NO policy -- all state->icon/page decisions happen in App.
 *        Temperatures are signed and scaled x10 (270 -> 27.0 degC).
 */
typedef struct {
    /* --- dashboard --- */
    uint16_t dc_voltage_x10;
    uint16_t dc_current_x10;
    uint16_t dc_power_x10_kw;     /* 0.1 kW (VD: 300 = 30.0 kW) */
    uint16_t bat_pack_volt_x10;
    uint16_t bat_cell_volt_x100;
    uint32_t charged_ah_x10;      /* CAPACITY */
    uint16_t ac_l1_v;
    uint16_t ac_l2_v;
    uint16_t ac_l3_v;
    int16_t  temp_battery_c_x10;
    int16_t  temp_charge_c_x10;
    int16_t  temp_jack_c_x10;
    uint16_t soc_pct;
    uint16_t status_icon;         /* DwinStatusIcon_e */
    uint16_t btn_mode;            /* DwinBtnMode_e */
    char     topbar_fault_code[8];/* "0000" or active fault code like "E006" */
    uint32_t charge_duration_s;   /* elapsed time since charge start in seconds */
    char     footer_time_str[16]; /* optional clock/time string "HH:MM:SS" (e.g. from RTC) */

    /* Battery Text Variable fields (empty string "" when no BMS -> DWIN blanks/hides) */
    char     soc_text[8];            /* e.g. "85 %" or "" */
    char     bat_pack_volt_text[8];  /* e.g. "52.1 V" or "" */
    char     bat_cell_volt_text[8];  /* e.g. "3.25 V" or "" */
    char     bat_cap_text[10];       /* e.g. "25.0 Ah" or "" */
    char     temp_battery_text[8];   /* e.g. "28.5 C" or "" */

    /* --- setting --- */
    uint32_t uptime_s;
    uint32_t total_charged_ah_x10;
    uint32_t total_energy_kwh_x10;
} DWIN_SystemData_t;

/** No-op today (BSP owns the UART); kept as the module's init seam. */
void DWIN_Init(void);

/**
 * @brief Write @p n_words 16-bit words to consecutive VPs starting at @p vp.
 *        One 0x82 frame, big-endian, no CRC.
 */
void DWIN_SendWords(uint16_t vp, const uint16_t *words, uint8_t n_words);

/**
 * @brief Write an ASCII/GBK string into a fixed @p field_words VP field, padded
 *        with 0x00 so the whole field is overwritten (no stale characters).
 */
void DWIN_SendString(uint16_t vp, const char *str, uint8_t field_words);

/**
 * @brief Switch the displayed page via VP_SYS_PIC_SET. Emits a frame only
 *        when @p page differs from the last one sent.
 */
void DWIN_SetPage(DwinPageId_e page);

/** One-shot: push the three identity strings on the Setting screen. */
void DWIN_SendSettingStrings(const char *hw_ver, const char *fw_ver,
                             const char *device_id);

/**
 * @brief Scatter-send: emits at most ONE frame per call, cycling through the
 *        dashboard/setting fields. Call from the ~50 ms HMI tick. Unchanged
 *        fields are skipped to keep the half-duplex bus quiet.
 */
void DWIN_UpdateData(const DWIN_SystemData_t *data);

/**
 * @brief Make the next full DWIN_UpdateData() scatter cycle send every field
 *        unconditionally (one-shot, regardless of which step it is on now).
 */
void DWIN_ForceFullRefresh(void);

/**
 * @brief Feed raw RS485 RX bytes (whatever BSP_RS485_Read() returned).
 */
void DWIN_ParseRX(const uint8_t *buf, uint16_t len);

/**
 * @brief Weak callback: the action button on the screen was pressed.
 */
void DWIN_OnActionButton(uint16_t keyval);

/**
 * @brief Push a wall-clock time to the panel's RTC (VP_SYS_RTC_SET).
 */
void DWIN_SetRTC(uint16_t year, uint8_t month, uint8_t day,
                  uint8_t hour, uint8_t minute, uint8_t second);

/**
 * @brief Push a new alarm entry into the 4-row FIFO table on Page 3.
 *        Older rows shift down (row 3 -> 4, 2 -> 3, 1 -> 2), and the
 *        oldest entry (row 4) is dropped.
 * @param time_str    "HH:MM:SS" (GBK/ASCII, up to 8 chars)
 * @param code_str    Alarm code like "E006", "W002" (GBK/ASCII, up to 8 chars)
 * @param desc_utf16  Pointer to Unicode UTF-16BE array (up to 32 chars)
 * @param desc_len    Character count of desc_utf16
 */
void DWIN_Alarm_Push(const char *time_str, const char *code_str,
                     const uint16_t *desc_utf16, uint8_t desc_len);

/**
 * @brief Clear all 4 alarm rows on the panel.
 */
void DWIN_Alarm_ClearAll(void);

#ifdef __cplusplus
}
#endif

#endif /* DWIN_PROTOCOL_H */
