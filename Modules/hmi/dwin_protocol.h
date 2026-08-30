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
 * (2026-08-30). If the panel stops recognising frames, swap these two
 * back to 0x5A / 0xA5 -- it is the only change needed (framing + parser
 * both read these macros). */
#define DWIN_HEADER_1   0xA5U
#define DWIN_HEADER_2   0x5AU
#define DWIN_CMD_WRITE  0x82U
#define DWIN_CMD_READ   0x83U

/**
 * @brief Snapshot of everything shown on the dashboard + setting screens.
 * @note  Built by the composition root (App/System/app_main.c) each HMI tick
 *        from the charge-controller / BMS / charger views. This module holds
 *        NO policy -- all state->icon/page decisions happen in App.
 *        Temperatures are signed and scaled x10 (270 -> 27.0 degC): the DGUS
 *        Data Variable control for 0x1030..0x1032 must be a signed integer
 *        with 1 decimal place. Negative values go as two's complement.
 */
typedef struct {
    /* --- dashboard --- */
    uint16_t dc_voltage_x10;
    uint16_t dc_current_x10;
    uint16_t dc_power_w;
    uint16_t bat_pack_volt_x10;
    uint16_t bat_cell_volt_x100;
    uint32_t charged_ah_x10;
    uint16_t ac_l1_v;
    uint16_t ac_l2_v;
    uint16_t ac_l3_v;
    int16_t  temp_battery_c_x10;
    int16_t  temp_charge_c_x10;
    int16_t  temp_jack_c_x10;
    uint16_t soc_pct;
    uint16_t status_icon;   /* DwinStatusIcon_e */
    uint16_t btn_mode;      /* DwinBtnMode_e */
    /* --- setting --- */
    uint32_t uptime_s;
} DWIN_SystemData_t;

/** No-op today (BSP owns the UART); kept as the module's init seam. */
void DWIN_Init(void);

/**
 * @brief Write @p n_words 16-bit words to consecutive VPs starting at @p vp.
 *        One 0x82 frame, big-endian, no CRC. Silently drops out-of-range
 *        requests (n_words 1..DWIN_TX_MAX_WORDS).
 */
void DWIN_SendWords(uint16_t vp, const uint16_t *words, uint8_t n_words);

/**
 * @brief Write an ASCII string into a fixed @p field_words VP field, padded
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
 *        Call once the panel is (re)ready, and on a periodic heartbeat, so a
 *        panel that booted late or brown-out-rebooted catches up without
 *        waiting for a value to change.
 */
void DWIN_ForceFullRefresh(void);

/**
 * @brief Feed raw RS485 RX bytes (whatever BSP_RS485_Read() returned).
 *        Parses 0x83 frames; on a non-zero VP_SYS_ACTION_BTN value it calls
 *        DWIN_OnActionButton() then writes 0 back to clear the VP.
 */
void DWIN_ParseRX(const uint8_t *buf, uint16_t len);

/**
 * @brief Weak callback: the action button on the screen was pressed.
 * @param keyval  raw Return-Key-Code value. The DGUS button is a single
 *        fixed-value control, so this really only means "pressed" -- the
 *        override in app_main.c decides start/stop/reset from
 *        ChargeController state, exactly like the physical PA15 button.
 */
void DWIN_OnActionButton(uint16_t keyval);

/**
 * @brief Push a wall-clock time to the panel's RTC (VP_SYS_RTC_SET).
 * @note  Implemented but intentionally not called yet -- the panel keeps its
 *        own time via its RTC IC. Wire this once a BSP_RTC wrapper exists
 *        (the STM32 LSE crystal is on the board but the RTC peripheral is
 *        not enabled in CubeMX yet). @p year is the full year, e.g. 2026.
 */
void DWIN_SetRTC(uint16_t year, uint8_t month, uint8_t day,
                 uint8_t hour, uint8_t minute, uint8_t second);

#ifdef __cplusplus
}
#endif

#endif /* DWIN_PROTOCOL_H */
