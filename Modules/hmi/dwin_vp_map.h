#ifndef DWIN_HMI_VP_MAP_H
#define DWIN_HMI_VP_MAP_H

/* ========================================================================= *
 * DWIN DGUS-II variable-pointer (VP) map for the charger HMI.               *
 * ========================================================================= *
 *
 * Firmware-side copy of the contract implemented by the DGUS touch/display
 * project under ui/ (ui/dwin_vp_map.h, ui/charger_pkg_dwin.html,
 * ui/DWIN_SET/). ui/ is a separate, concurrently-edited deliverable and is
 * not a buildable source layer, so firmware keeps its own copy here.
 *
 *   >>> KEEP THIS FILE IN SYNC WITH THE DGUS PROJECT BY HAND. <<<
 *   A mismatch makes the screen show garbage.
 *
 * Wire format (based on T5L_DGUSII guide V2.9 sec 4.2):
 *   MCU -> panel : <H1> <H2> <len> 82 <vp_hi> <vp_lo> <payload...>
 *   panel -> MCU : <H1> <H2> <len> 83 <vp_hi> <vp_lo> <n_words> <payload...>
 *   Header <H1><H2> = DWIN_HEADER_1/DWIN_HEADER_2 in dwin_protocol.h. The
 *   DGUS-stock value is 5A A5; THIS PROJECT IS BUILT WITH A5 5A (user
 *   request 2026-08-30). <len> counts the command byte + VP + payload. CRC
 *   is OFF (DGUS CFG bit 0x05.7 = 0) -- firmware sends and expects no CRC.
 * All values big-endian. A 32-bit value spans two consecutive VPs, high
 * word at the lower address.
 */

/* --- System VPs (guide sec 5.1) --- */
#define VP_SYS_PIC_SET       0x0084U  /* page switch: write word 0x5A01 then the page id */
#define VP_SYS_RTC_SET       0x009CU  /* RTC set (needs a panel RTC IC) -- see DWIN_SetRTC() */
#define VP_SYS_BUZZER        0x00A0U  /* Music_Play_Set: low byte = beep duration x 8 ms */

/* --- Dashboard: Output DC (0x1000) --- */
#define VP_DC_VOLTAGE        0x1000U  /* u16, 0.1 V  (521 -> 52.1 V) */
#define VP_DC_CURRENT        0x1001U  /* u16, 0.1 A */
#define VP_DC_POWER          0x1002U  /* u16, 1 W */

/* --- Dashboard: Battery (0x1010) --- */
#define VP_BAT_PACK_VOLT     0x1010U  /* u16, 0.1 V */
#define VP_BAT_CELL_VOLT     0x1011U  /* u16, 0.01 V (325 -> 3.25 V) */
#define VP_BAT_CHARGED_AH    0x1012U  /* u32 @ 0x1012..0x1013, 0.1 Ah */

/* --- Dashboard: Input AC (0x1020) --- */
#define VP_AC_PHASE_L1       0x1020U  /* u16, 1 V */
#define VP_AC_PHASE_L2       0x1021U
#define VP_AC_PHASE_L3       0x1022U

/* --- Dashboard: Temperature (0x1030), i16 signed, 0.1 degC (270 = 27.0),
 *     unavailable -> 0. DGUS control: signed int, 1 decimal place. --- */
#define VP_TEMP_BATTERY      0x1030U  /* BMS max cell temp */
#define VP_TEMP_CHARGE       0x1031U  /* hottest module DC-DC temp (CAN), = PC app max_temp_dcdc */
#define VP_TEMP_JACK         0x1032U  /* hottest of the 4 connector NTCs (PA0..PA3) */

/* --- Dashboard: centre status + action button (0x1040) --- */
#define VP_SOC_VALUE         0x1040U  /* MCU->panel: u16, 0..100 % */
#define VP_SYS_STATUS_ICON   0x1041U  /* MCU->panel: u16 DwinStatusIcon_e -- status-box Variable Icon */
#define VP_SYS_BTN_ICON      0x1042U  /* MCU->panel: u16 DwinBtnMode_e (0..3) -- button-label Variable Icon */
#define VP_SYS_BTN_KEY       0x1043U  /* panel->MCU: Return-Key-Code upload on every press (fixed keycode,
                                       * value ignored). MCU never writes this VP. */

/* --- Setting screen (0x1100) --- */
#define VP_SET_HW_VER        0x1100U  /* ASCII, 8 VP / 16 chars */
#define VP_SET_FW_VER        0x1108U  /* ASCII, 8 VP / 16 chars */
#define VP_SET_DEVICE_ID     0x1110U  /* ASCII, 8 VP / 16 chars */
#define VP_SET_UPTIME        0x1118U  /* u32 @ 0x1118..0x1119, seconds */

#define VP_SET_STR_WORDS     8U       /* field width of each string VP above */

/* --- Alarm table (0x1200) -- Phase 2, firmware does not write this yet --- *
 * Row layout (20 VP / row): Time +0x00 (4 VP), Level +0x04 (1 VP icon),
 * Code +0x05 (4 VP), Desc +0x09 (11 VP). 5 rows -> 0x1200..0x12A0. */
#define VP_ALARM_ROW_BASE    0x1200U
#define VP_ALARM_ROW_STRIDE  0x0020U

/* Page ids -- must match the DGUS project's picture order (ui/ Images View:
 * 00 logo, 01 dashboard, 02 setting, 03 alarm). */
typedef enum {
    DWIN_PAGE_LOGO    = 0,
    DWIN_PAGE_DASH    = 1,
    DWIN_PAGE_SETTING = 2,
    DWIN_PAGE_ALARM   = 3,
} DwinPageId_e;

/* VP_SYS_STATUS_ICON values (status-box Variable Icon on the dashboard). */
typedef enum {
    DWIN_STATUS_READY    = 0,
    DWIN_STATUS_STARTING = 1,
    DWIN_STATUS_CHARGING = 2,
    DWIN_STATUS_COMPLETE = 3,
    DWIN_STATUS_ERROR    = 4,
    DWIN_STATUS_OFFLINE  = 5,
} DwinStatusIcon_e;

/* VP_SYS_BTN_ICON values (button-label Variable Icon). */
typedef enum {
    DWIN_BTN_START    = 0,
    DWIN_BTN_STOP     = 1,
    DWIN_BTN_RESET    = 2,
    DWIN_BTN_DISABLED = 3,
} DwinBtnMode_e;

#endif /* DWIN_HMI_VP_MAP_H */
