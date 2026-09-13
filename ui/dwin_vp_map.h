#ifndef DWIN_VP_MAP_H
#define DWIN_VP_MAP_H

/* ========================================================================= *
 * DWIN DGUS II - VARIABLE POINTER (VP) MAP FOR CHARGER FIRMWARE             *
 * Layout 480x272 - Cập nhật theo charger_pkg.html & config DWIN thực tế     *
 * ========================================================================= */

// --- 1. SYSTEM CONTROL (HỆ THỐNG DWIN) ---
#define VP_SYS_PIC_ID          0x0084  // Đổi trang màn hình (0:Logo, 1:Dash, 2:Setting, 3:Alarm)
#define VP_SYS_RTC_UPDATE      0x009C  // Cập nhật RTC hệ thống (Gửi 6 bytes: YY MM DD HH MM SS)
#define VP_SYS_BUZZER          0x00A0  // Điều khiển còi (Gửi 0x0040 để bíp)

// --- 2. DASHBOARD - OUTPUT DC (0x1000) ---
#define VP_DC_VOLTAGE          0x1000  // u16, 1 Decimal (VD: 521 = 52.1V)
#define VP_DC_CURRENT          0x1001  // u16, 1 Decimal (VD: 105 = 10.5A)
#define VP_DC_POWER            0x1002  // u16, 1 Decimal (VD: 300 = 30.0kW - HTML mới đơn vị kW)

// --- 3. DASHBOARD - BATTERY (0x1010) ---
// All three battery values are Text Displays, 8 bytes / 4 VP each.
#define VP_BAT_PACK_VOLT       0x1010  // Text: pack voltage value, e.g. "52.8"
#define VP_BAT_CELL_VOLT       0x1014  // Text: max cell voltage in volts, 3 decimals, e.g. "3.315"
#define VP_BAT_CHARGED_AH      0x1018  // Text: remaining capacity, e.g. "82.0"

// --- 4. DASHBOARD - INPUT AC (0x1020) ---
#define VP_AC_PHASE_L1         0x1020  // u16, 0 Decimal (VD: 225 = 225V)
#define VP_AC_PHASE_L2         0x1021  // u16, 0 Decimal (VD: 226 = 226V)
#define VP_AC_PHASE_L3         0x1022  // u16, 0 Decimal (VD: 224 = 224V)

// --- 5. DASHBOARD - TEMPERATURE (0x1030) ---
#define VP_TEMP_BATTERY        0x1030  // i16 Signed, 1 Decimal (VD: 280 = 28.0°C)
#define VP_TEMP_CHARGE         0x1031  // i16 Signed, 1 Decimal (VD: 350 = 35.0°C)
#define VP_TEMP_JACK           0x1032  // i16 Signed, 1 Decimal (VD: 300 = 30.0°C)

// --- 6. DASHBOARD - CENTER STATUS, BUTTON & FOOTER (0x1040) ---
#define VP_SOC_VALUE           0x1040  // u16 (0 - 100%)
#define VP_SYS_STATUS_ICON     0x1041  // Var Icon (0:READY, 1:STARTING, 2:CHARGING, 3:COMPLETE, 4:ERROR, 5:OFFLINE)
#define VP_SYS_BTN_ICON        0x1042  // Var Icon nhãn nút (0:START, 1:STOP, 2:RESET)
#define VP_SYS_ACTION_KEY      0x1043  // Return Key Code (Phím cảm ứng Start/Stop, panel -> MCU)
#define VP_TOPBAR_FAULT_CODE   0x1044  // Shared fault-code Text (GBK, 8 bytes): shown on Home and Page 07
#define VP_CHG_DURATION        0x1050  // Text (GBK, 16 bytes = 8 VP @ 0x1050..0x1057): Thời gian sạc HH:MM:SS

// --- 7. SETTING SCREEN (0x1100) ---
// Tất cả chuỗi hiển thị ở đây đều dùng mã GBK (ASCII)
#define VP_SET_HW_VER          0x1100  // Text (GBK, 8 bytes = 4 VP): "HW V1.2"
#define VP_SET_FW_VER          0x1108  // Text (GBK, 8 bytes = 4 VP): "FW V2.04"
#define VP_SET_DEVICE_ID       0x1110  // Text (GBK, 8 bytes = 4 VP): "PKG-0001"
#define VP_SET_TOTAL_CHARGED   0x1118  // Text (GBK, 16 bytes = 8 VP): "12500.5 Ah"
#define VP_SET_TOTAL_ENERGY    0x1120  // Text (GBK, 16 bytes = 8 VP): "685.2 kWh"
#define VP_SET_UPTIME          0x1128  // Text (GBK, 16 bytes = 8 VP): "125:32:18" (HHH:MM:SS)
#define VP_SET_LOGIN_KEY       0x1130  // Return Key: open admin Login page

// --- 8. LOGIN + PRE-CHARGE (Page 06 / Page 07) ---
#define VP_LOGIN_PIN_TEXT          0x1500  // Text, 8 bytes / 4 VP: masked ******
#define VP_LOGIN_KEY               0x1504  // Return Key: digits, DEL, OK, Back
#define VP_PRECHARGE_VOLTAGE_TEXT  0x1510  // Text, 8 bytes / 4 VP: system output voltage
#define VP_PRECHARGE_CURRENT_TEXT  0x1514  // Text, 8 bytes / 4 VP: system total current
#define VP_PRECHARGE_STATUS_ICON   0x1518  // Variable Icon: 27.icl (0:Ready, 1:Pre, 2:Error)
#define VP_PRECHARGE_BTN_ICON      0x1519  // Variable Icon: 26.icl (0:Start, 1:Stop, 2:Reset)
#define VP_PRECHARGE_ACTION_KEY    0x151A  // Return Key: 0x0001 (Action), 0x0002 (Back)

#define DWIN_LOGIN_KEY_DIGIT_0     0x0030  // ASCII '0'
#define DWIN_LOGIN_KEY_DIGIT_9     0x0039  // ASCII '9'
#define DWIN_LOGIN_KEY_DELETE      0x00F0  // DWIN DEL
#define DWIN_LOGIN_KEY_OK          0x00F1  // DWIN OK
#define DWIN_LOGIN_KEY_BACK        0x00F2  // DWIN BACK
#define DWIN_PRECHARGE_KEY_ACTION  0x0001
#define DWIN_PRECHARGE_KEY_BACK    0x0002
#define DWIN_PRECHARGE_KEY_START   0x0001
#define DWIN_PRECHARGE_KEY_STOP    0x0001
#define DWIN_SETTING_KEY_LOGIN     0x0301

// --- 8. ALARM TABLE (0x1200) - BẢNG 12 LỖI XOAY VÒNG (3 Trang x 4 Dòng) ---
/* Cấu trúc 1 dòng (48 VP = 0x30 stride):
 *  - Time: +0x00 (Text GBK,     Text_Length = 8  bytes -> 4 VP)
 *  - Code: +0x04 (Text GBK,     Text_Length = 8  bytes -> 4 VP)
 *  - Desc: +0x08 (Text UNICODE, Text_Length = 64 bytes -> 32 VP, tối đa 32 ký tự Tiếng Việt UTF-16BE)
 */
#define VP_ALARM_ROW_BASE      0x1200U
#define VP_ALARM_ROW_STRIDE    0x0030U  // 48 VP mỗi dòng
#define VP_ALARM_ROW_COUNT     12U      // 12 dòng xoay vòng (FIFO)

#define VP_ALARM_ROW_1         0x1200
#define VP_ALARM_ROW_2         0x1230
#define VP_ALARM_ROW_3         0x1260
#define VP_ALARM_ROW_4         0x1290
#define VP_ALARM_ROW_5         0x12C0
#define VP_ALARM_ROW_6         0x12F0
#define VP_ALARM_ROW_7         0x1320
#define VP_ALARM_ROW_8         0x1350
#define VP_ALARM_ROW_9         0x1380
#define VP_ALARM_ROW_10        0x13B0
#define VP_ALARM_ROW_11        0x13E0
#define VP_ALARM_ROW_12        0x1410

// Offset trong từng dòng cảnh báo:
#define ALARM_OFFSET_TIME      0x0000   // 4 VP (GBK, 8 bytes)
#define ALARM_OFFSET_CODE      0x0004   // 4 VP (GBK, 8 bytes)
#define ALARM_OFFSET_DESC      0x0008   // 32 VP (UNICODE Tiếng Việt, 64 bytes)

#endif // DWIN_VP_MAP_H
