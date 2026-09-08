#ifndef DWIN_HMI_VP_MAP_H
#define DWIN_HMI_VP_MAP_H

/* ========================================================================= *
 * DWIN DGUS-II variable-pointer (VP) map for the charger HMI.               *
 * Layout 480x272 - Cập nhật theo charger_pkg.html & config DWIN thực tế     *
 * ========================================================================= *
 *
 * Firmware-side copy of the contract implemented by the DGUS touch/display
 * project under ui/ (ui/dwin_vp_map.h, ui/charger_pkg.html, ui/DWIN_SET/).
 *
 * Wire format (based on T5L_DGUSII guide V2.9 sec 4.2):
 *   MCU -> panel : <H1> <H2> <len> 82 <vp_hi> <vp_lo> <payload...>
 *   panel -> MCU : <H1> <H2> <len> 83 <vp_hi> <vp_lo> <n_words> <payload...>
 *   Header <H1><H2> = DWIN_HEADER_1/DWIN_HEADER_2 in dwin_protocol.h.
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
#define VP_DC_POWER          0x1002U  /* u16, 0.1 kW (300 -> 30.0 kW - new HTML format) */

/* --- Dashboard: Battery (0x1010) --- */
#define VP_BAT_PACK_VOLT     0x1010U  /* u16, 0.1 V (legacy numeric) */
#define VP_BAT_CELL_VOLT     0x1011U  /* u16, 0.01 V (325 -> 3.25 V, legacy numeric) */
#define VP_BAT_CHARGED_AH    0x1012U  /* u16 @ 0x1012, 0.1 Ah (Label: CAPACITY, legacy numeric) */

/* Dashboard: Battery Text format (for DWIN Text Variable, blanks when no BMS) */
#define VP_BAT_PACK_VOLT_TEXT  0x1010U  /* ASCII/GBK, 4 VP / 8 chars (e.g. "52.1 V" or "") */
#define VP_BAT_CELL_VOLT_TEXT  0x1014U  /* ASCII/GBK, 4 VP / 8 chars (e.g. "3.25 V" or "") */
#define VP_BAT_CHARGED_AH_TEXT 0x1018U  /* ASCII/GBK, 4 VP / 8 chars (e.g. "25.0 Ah" or "") */

/* --- Dashboard: Input AC (0x1020) --- */
#define VP_AC_PHASE_L1       0x1020U  /* u16, 1 V */
#define VP_AC_PHASE_L2       0x1021U
#define VP_AC_PHASE_L3       0x1022U

/* --- Dashboard: Temperature (0x1030), i16 signed, 0.1 degC (0.0 format) --- */
#define VP_TEMP_BATTERY      0x1030U  /* BMS max cell temp (0.1 degC, legacy numeric) */
#define VP_TEMP_CHARGE       0x1031U  /* hottest module DC-DC temp (0.1 degC) */
#define VP_TEMP_JACK         0x1032U  /* hottest of the 4 connector NTCs (0.1 degC) */
#define VP_TEMP_BATTERY_TEXT 0x1034U  /* ASCII/GBK, 4 VP / 8 chars (e.g. "28.5 C" or "") */

/* --- Dashboard: centre status + action button + fault code + charge duration (0x1040) --- */
#define VP_SOC_VALUE         0x1040U  /* MCU->panel: u16, 0..100 % (legacy numeric) */
#define VP_SYS_STATUS_ICON   0x1041U  /* MCU->panel: u16 DwinStatusIcon_e -- status-box Variable Icon */
#define VP_SYS_BTN_ICON      0x1042U  /* MCU->panel: u16 DwinBtnMode_e (0..3) -- button-label Variable Icon */
#define VP_SYS_BTN_KEY       0x1043U  /* panel->MCU: Return-Key-Code upload on every press */
#define VP_TOPBAR_FAULT_CODE 0x1044U  /* MCU->panel: Text GBK, 8 bytes = 4 VP @ 0x1044..0x1047 (VD: "0000", "E006") */
#define VP_SOC_TEXT          0x1048U  /* MCU->panel: Text GBK, 8 bytes = 4 VP @ 0x1048..0x104B (e.g. "85 %" or "") */
#define VP_CHG_DURATION      0x1050U  /* MCU->panel: Text GBK, 16 bytes = 8 VP @ 0x1050..0x1057 (VD: "01:25:34") */

/* --- Setting screen (0x1100) --- */
#define VP_SET_HW_VER        0x1100U  /* ASCII/GBK, 4 VP / 8 chars (DGUS config len=8) */
#define VP_SET_FW_VER        0x1108U  /* ASCII/GBK, 4 VP / 8 chars */
#define VP_SET_DEVICE_ID     0x1110U  /* ASCII/GBK, 4 VP / 8 chars */
#define VP_SET_TOTAL_CHARGED 0x1118U  /* ASCII/GBK, 8 VP / 16 chars: "12500.5 Ah" */
#define VP_SET_TOTAL_ENERGY  0x1120U  /* ASCII/GBK, 8 VP / 16 chars: "685.2 kWh" */
#define VP_SET_UPTIME        0x1128U  /* ASCII/GBK, 8 VP / 16 chars: "125:32:18" (HHH:MM:SS) */

#define VP_SET_ID_WORDS      4U       /* 4 VP = 8 bytes max for HW, FW, Device ID */
#define VP_SET_STR_WORDS     8U       /* default field width */

/* --- Alarm table (0x1200) - 4 ROWS FIFO, LEVEL column removed --- *
 * Row layout (48 VP / row):
 *   Time: +0x00 (Text GBK,     Text_Length = 8  bytes -> 4 VP)
 *   Code: +0x04 (Text GBK,     Text_Length = 8  bytes -> 4 VP)
 *   Desc: +0x08 (Text UNICODE, Text_Length = 64 bytes -> 32 VP, Vietnamese UTF-16BE)
 * 4 rows -> 0x1200..0x12BF */
#define VP_ALARM_ROW_BASE    0x1200U
#define VP_ALARM_ROW_STRIDE  0x0030U  /* 48 VP stride */
#define VP_ALARM_ROW_COUNT   4U       /* 4 rows circular buffer */

#define VP_ALARM_ROW_1       0x1200U
#define VP_ALARM_ROW_2       0x1230U
#define VP_ALARM_ROW_3       0x1260U
#define VP_ALARM_ROW_4       0x1290U

#define ALARM_OFFSET_TIME    0x0000U  /* 4 VP (GBK) */
#define ALARM_OFFSET_CODE    0x0004U  /* 4 VP (GBK) */
#define ALARM_OFFSET_DESC    0x0008U  /* 32 VP (UNICODE Vietnamese) */

/* Page ids -- must match the DGUS project's picture order:
 * 00 logo, 01 dashboard, 02 setting, 03 alarm. */
typedef enum {
    DWIN_PAGE_LOGO    = 0,
    DWIN_PAGE_DASH    = 1,
    DWIN_PAGE_SETTING = 2,
    DWIN_PAGE_ALARM   = 3,
} DwinPageId_e;

/* VP_SYS_STATUS_ICON values (status-box Variable Icon on the dashboard).
 * DGUS 28.icl has exactly 5 images (0..4). Values >= 5 render transparent/blank. */
typedef enum {
    DWIN_STATUS_READY    = 0,
    DWIN_STATUS_STARTING = 1,
    DWIN_STATUS_CHARGING = 2,
    DWIN_STATUS_COMPLETE = 3,
    DWIN_STATUS_ERROR    = 4,
} DwinStatusIcon_e;

/* VP_SYS_BTN_ICON values (button-label Variable Icon).
 * DGUS 25.icl has exactly 3 images (0..2). Values >= 3 render transparent/blank. */
typedef enum {
    DWIN_BTN_START    = 0,
    DWIN_BTN_STOP     = 1,
    DWIN_BTN_RESET    = 2,
} DwinBtnMode_e;

#endif /* DWIN_HMI_VP_MAP_H */
