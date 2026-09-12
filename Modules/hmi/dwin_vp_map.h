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

/* --- Dashboard text fields ---
 * DWIN VP is a 16-bit word address: one 8-byte Text Display occupies
 * 4 consecutive VPs. Text_Length is measured in bytes, not characters. */
#define DWIN_TEXT_8_BYTES_WORDS  4U

/* Output DC */
#define VP_DC_VOLTAGE        0x1000U  /* Text GBK/ASCII, 8 bytes, 0x1000..1003 */
#define VP_DC_CURRENT        0x1004U  /* Text GBK/ASCII, 8 bytes, 0x1004..1007 */
#define VP_DC_POWER          0x1008U  /* Text GBK/ASCII, 8 bytes, 0x1008..100B */

/* --- Dashboard: Battery (0x1010) --- */
#define VP_BAT_PACK_VOLT_TEXT  0x1010U  /* Text GBK/ASCII, 8 bytes, 0x1010..1013 */
#define VP_BAT_CELL_VOLT_TEXT  0x1014U  /* Text GBK/ASCII, 8 bytes: max cell voltage in 4-digit mV (e.g. "3315") */
#define VP_BAT_CHARGED_AH_TEXT 0x1018U  /* Text GBK/ASCII, 8 bytes, 0x1018..101B */

/* --- Dashboard: Input AC (0x1020) --- */
#define VP_AC_PHASE_L1       0x1020U  /* Text GBK/ASCII, 8 bytes, 0x1020..1023 */
#define VP_AC_PHASE_L2       0x1024U  /* Text GBK/ASCII, 8 bytes, 0x1024..1027 */
#define VP_AC_PHASE_L3       0x1028U  /* Text GBK/ASCII, 8 bytes, 0x1028..102B */

/* --- Dashboard: Temperature (0x1030), text fields, 8 bytes each --- */
#define VP_TEMP_BATTERY_TEXT 0x1030U  /* Text GBK/ASCII, 8 bytes, 0x1030..1033 */
#define VP_TEMP_CHARGE_TEXT  0x1034U  /* Text GBK/ASCII, 8 bytes, 0x1034..1037 */
#define VP_TEMP_JACK_TEXT    0x1038U  /* Text GBK/ASCII, 8 bytes, 0x1038..103B */

/* --- Dashboard: status + action button + fault code + charge duration --- */
#define VP_SYS_STATUS_ICON   0x1041U  /* MCU->panel: u16 DwinStatusIcon_e -- status-box Variable Icon */
#define VP_SYS_BTN_ICON      0x1042U  /* MCU->panel: u16 DwinBtnMode_e (0..3) -- button-label Variable Icon */
#define VP_SYS_BTN_KEY       0x1043U  /* panel->MCU: Return-Key-Code upload on every press */
#define VP_TOPBAR_FAULT_CODE 0x1044U  /* MCU->panel: shared fault-code Text, 8 bytes; shown on Home and Page 07 */
#define DWIN_SOC_TEXT_VP     0x1048U  /* MCU->panel: Text GBK, 8 bytes = 4 VP @ 0x1048..0x104B */
#define VP_CHG_DURATION      0x1050U  /* MCU->panel: Text GBK, 16 bytes = 8 VP @ 0x1050..0x1057 (VD: "01:25:34") */

/* SOC Text Display style pointer contract from the DWIN project. The color
 * field is three WORDs after the SP, not a byte or bit offset. */
#define DWIN_SOC_SP                   0x8000U
#define DWIN_TEXT_COLOR_OFFSET_WORDS  3U
#define DWIN_SOC_COLOR_ADDR           (DWIN_SOC_SP + DWIN_TEXT_COLOR_OFFSET_WORDS)

#define DWIN_SOC_COLOR_RGB565_UNAVAILABLE 0x8410U /* neutral gray */
#define DWIN_SOC_COLOR_RGB565_CRITICAL    0xF800U /* red */
#define DWIN_SOC_COLOR_RGB565_LOW         0xFD20U /* orange */
#define DWIN_SOC_COLOR_RGB565_MEDIUM      0xFFE0U /* yellow */
#define DWIN_SOC_COLOR_RGB565_NORMAL      0x07E0U /* green */

typedef enum {
    DWIN_SOC_COLOR_UNAVAILABLE = 0,
    DWIN_SOC_COLOR_CRITICAL,
    DWIN_SOC_COLOR_LOW,
    DWIN_SOC_COLOR_MEDIUM,
    DWIN_SOC_COLOR_NORMAL
} DwinSocColor_e;

/* --- Setting screen (0x1100) --- */
#define VP_SET_HW_VER        0x1100U  /* ASCII/GBK, 4 VP / 8 chars (DGUS config len=8) */
#define VP_SET_FW_VER        0x1108U  /* ASCII/GBK, 4 VP / 8 chars */
#define VP_SET_DEVICE_ID     0x1110U  /* ASCII/GBK, 4 VP / 8 chars */
#define VP_SET_TOTAL_CHARGED 0x1118U  /* ASCII/GBK, 8 VP / 16 chars: "12500.5 Ah" */
#define VP_SET_TOTAL_ENERGY  0x1120U  /* ASCII/GBK, 8 VP / 16 chars: "685.2 kWh" */
#define VP_SET_UPTIME        0x1128U  /* ASCII/GBK, 8 VP / 16 chars: "125:32:18" (HHH:MM:SS) */
#define VP_SET_LOGIN_KEY     0x1130U  /* panel->MCU: Return Key -> admin Login page */

#define VP_SET_ID_WORDS      4U       /* 4 VP = 8 bytes max for HW, FW, Device ID */
#define VP_SET_STR_WORDS     8U       /* default field width */

/* --- Login + Pre-Charge (Page 06 / Page 07) --- */
#define VP_LOGIN_PIN_TEXT          0x1500U /* MCU->panel: Text, 8 bytes / 4 VP */
#define VP_LOGIN_KEY               0x1504U /* panel->MCU: Return Key */
#define VP_PRECHARGE_VOLTAGE_TEXT  0x1510U /* MCU->panel: Text, 8 bytes / 4 VP */
#define VP_PRECHARGE_CURRENT_TEXT  0x1514U /* MCU->panel: Text, 8 bytes / 4 VP */
#define VP_PRECHARGE_STATUS_ICON   0x1518U /* MCU->panel: Variable Icon 27.icl (0:Ready, 1:Pre, 2:Error) */
#define VP_PRECHARGE_BTN_ICON      0x1519U /* MCU->panel: Variable Icon 26.icl (0:Start, 1:Stop, 2:Reset) */
#define VP_PRECHARGE_ACTION_KEY    0x151AU /* panel->MCU: Return Key (0x0001: Action, 0x0002: Back) */

/* Key codes for Page 06 Login keypad */
#define DWIN_LOGIN_KEY_DIGIT_0     0x0030U /* ASCII '0' */
#define DWIN_LOGIN_KEY_DIGIT_1     0x0031U /* ASCII '1' */
#define DWIN_LOGIN_KEY_DIGIT_2     0x0032U /* ASCII '2' */
#define DWIN_LOGIN_KEY_DIGIT_3     0x0033U /* ASCII '3' */
#define DWIN_LOGIN_KEY_DIGIT_4     0x0034U /* ASCII '4' */
#define DWIN_LOGIN_KEY_DIGIT_5     0x0035U /* ASCII '5' */
#define DWIN_LOGIN_KEY_DIGIT_6     0x0036U /* ASCII '6' */
#define DWIN_LOGIN_KEY_DIGIT_7     0x0037U /* ASCII '7' */
#define DWIN_LOGIN_KEY_DIGIT_8     0x0038U /* ASCII '8' */
#define DWIN_LOGIN_KEY_DIGIT_9     0x0039U /* ASCII '9' */
#define DWIN_LOGIN_KEY_DELETE      0x00F0U /* DWIN DEL */
#define DWIN_LOGIN_KEY_OK          0x00F1U /* DWIN OK */
#define DWIN_LOGIN_KEY_BACK        0x00F2U /* DWIN BACK */

/* Key codes for Page 07 Pre-charge controls */
#define DWIN_PRECHARGE_KEY_ACTION  0x0001U /* Start / Stop / Reset */
#define DWIN_PRECHARGE_KEY_BACK    0x0002U /* Back to Home */
#define DWIN_PRECHARGE_KEY_START   0x0001U /* Legacy alias */
#define DWIN_PRECHARGE_KEY_STOP    0x0001U /* Legacy alias */
#define DWIN_SETTING_KEY_LOGIN     0x0301U

/* Status icon values for 27.icl (Precharge status) */
typedef enum {
    DWIN_PRECHARGE_STATUS_READY  = 0,
    DWIN_PRECHARGE_STATUS_ACTIVE = 1,
    DWIN_PRECHARGE_STATUS_ERROR  = 2,
} DwinPrechargeStatusMode_e;

/* Button icon values for 26.icl (Precharge button) */
typedef enum {
    DWIN_PRECHARGE_BTN_START = 0,
    DWIN_PRECHARGE_BTN_STOP  = 1,
    DWIN_PRECHARGE_BTN_RESET = 2,
} DwinPrechargeBtnMode_e;

/* --- Alarm table (0x1200) - 12 ROWS FIFO (3 Pages x 4 Rows), LEVEL column removed --- *
 * Row layout (48 VP / row):
 *   Time: +0x00 (Text GBK,     Text_Length = 8  bytes -> 4 VP)
 *   Code: +0x04 (Text GBK,     Text_Length = 8  bytes -> 4 VP)
 *   Desc: +0x08 (Text UNICODE, Text_Length = 64 bytes -> 32 VP, Vietnamese UTF-16BE)
 * 12 rows -> 0x1200..0x143F */
#define VP_ALARM_ROW_BASE    0x1200U
#define VP_ALARM_ROW_STRIDE  0x0030U  /* 48 VP stride */
#define VP_ALARM_ROW_COUNT   12U      /* 12 rows circular buffer across 3 alarm pages */

#define VP_ALARM_ROW_1       0x1200U
#define VP_ALARM_ROW_2       0x1230U
#define VP_ALARM_ROW_3       0x1260U
#define VP_ALARM_ROW_4       0x1290U
#define VP_ALARM_ROW_5       0x12C0U
#define VP_ALARM_ROW_6       0x12F0U
#define VP_ALARM_ROW_7       0x1320U
#define VP_ALARM_ROW_8       0x1350U
#define VP_ALARM_ROW_9       0x1380U
#define VP_ALARM_ROW_10      0x13B0U
#define VP_ALARM_ROW_11      0x13E0U
#define VP_ALARM_ROW_12      0x1410U

#define ALARM_OFFSET_TIME    0x0000U  /* 4 VP (GBK) */
#define ALARM_OFFSET_CODE    0x0004U  /* 4 VP (GBK) */
#define ALARM_OFFSET_DESC    0x0008U  /* 32 VP (UNICODE Vietnamese) */

/* Page ids -- must match the DGUS project's picture order:
 * 00 logo, 01 dashboard, 02 setting, 03..05 alarm, 06 login, 07 pre-charge. */
typedef enum {
    DWIN_PAGE_LOGO    = 0,
    DWIN_PAGE_DASH    = 1,
    DWIN_PAGE_SETTING = 2,
    DWIN_PAGE_ALARM   = 3,
    DWIN_PAGE_LOGIN   = 6,
    DWIN_PAGE_PRECHARGE = 7,
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
