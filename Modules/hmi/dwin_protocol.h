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
 *        Dashboard measurements are fixed-width text fields. The composition
 *        root writes "---" when a source is offline or the individual value
 *        is invalid; valid values use ASCII-compatible GBK text.
 */
typedef struct {
    /* --- dashboard --- */
    char     dc_voltage_text[8];
    char     dc_current_text[8];
    char     dc_power_text[8];
    char     bat_pack_volt_text[8];
    char     bat_cell_volt_text[8];
    char     bat_cap_text[10];
    char     ac_l1_text[8];
    char     ac_l2_text[8];
    char     ac_l3_text[8];
    char     temp_battery_text[8];
    char     temp_charge_text[8];
    char     temp_jack_text[8];
    char     soc_text[8];
    uint16_t status_icon;         /* DwinStatusIcon_e */
    uint16_t btn_mode;            /* DwinBtnMode_e */
    char     topbar_fault_code[8];/* "0000" or active fault code like "E006" */
    uint32_t charge_duration_s;   /* elapsed time since charge start in seconds */
    char     footer_time_str[16]; /* optional clock/time string "HH:MM:SS" (e.g. from RTC) */

    /* --- pre-charge page --- */
    char     precharge_voltage_text[8];
    char     precharge_current_text[8];
    uint16_t precharge_status_mode;/* DwinPrechargeStatusMode_e */
    uint16_t precharge_btn_mode;   /* DwinPrechargeBtnMode_e */

    /* Text fields use ASCII-compatible GBK for values and "---" for
     * unavailable data. Dashboard units are drawn by the DWIN project;
     * SOC is the only field whose text includes "%" (for example "50%"). */

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
 * @brief Send 0x83 read request to DWIN RAM for n_words at vp.
 */
void DWIN_SendReadRequest(uint16_t vp, uint8_t n_words);

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

/** Send the panel's T5L software-reset command. */
void DWIN_SendSoftwareReset(void);

/** Invalidate MCU-side synchronization caches after a panel restart. */
void DWIN_InvalidateSyncState(void);

/** One-shot: push the three identity strings on the Setting screen. */
void DWIN_SendSettingStrings(const char *hw_ver, const char *fw_ver,
                             const char *device_id);

/**
 * @brief Trigger the panel's onboard buzzer for a duration of duration_x8ms * 8 ms.
 */
void DWIN_Beep(uint8_t duration_x8ms);

/** Total scatter steps in one full round-robin dashboard update cycle. */
#define DWIN_SCATTER_STEP_COUNT  11U

/**
 * @brief Scatter-send: cycles through the dashboard/setting field groups.
 *        Contiguous fields (e.g. DC Voltage, Current, Power @ 0x1000..0x100B)
 *        are coalesced into atomic multi-word frames to minimize RS485
 *        turnaround overhead. Unchanged fields/groups are diff-suppressed to
 *        keep the half-duplex bus quiet.
 */
void DWIN_UpdateData(const DWIN_SystemData_t *data);

/**
 * @brief Make the next full DWIN_UpdateData() scatter cycle send every field
 *        unconditionally (one-shot, regardless of which step it is on now).
 */
void DWIN_ForceFullRefresh(void);

/**
 * @brief Set the SOC Text Display foreground color through its SP contract.
 *        The protocol layer maps the semantic color to RGB565 and writes one
 *        WORD at DWIN_SOC_SP + DWIN_TEXT_COLOR_OFFSET_WORDS.
 */
void DWIN_SetSocColor(DwinSocColor_e color);

/**
 * @brief Feed raw RS485 RX bytes (whatever BSP_RS485_Read() returned).
 */
void DWIN_ParseRX(const uint8_t *buf, uint16_t len);

/**
 * @brief Weak callback: the action button on the screen was pressed.
 */
void DWIN_OnActionButton(uint16_t keyval);

/** Weak callback for non-dashboard Return Key controls. */
void DWIN_OnKeyEvent(uint16_t vp, uint16_t keyval);

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
