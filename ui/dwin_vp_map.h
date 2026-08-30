#ifndef DWIN_VP_MAP_H
#define DWIN_VP_MAP_H

/* ========================================================================= *
 * DWIN DGUS II - VARIABLE POINTER (VP) MAP FOR CHARGER FIRMWARE             *
 * ========================================================================= */

// --- 1. SYSTEM CONTROL (HỆ THỐNG) ---
#define VP_SYS_PIC_ID          0x0084  // Đổi trang màn hình (0:Dash, 1:Set, 2:Alarm)
#define VP_SYS_RTC_UPDATE      0x009C  // Cập nhật giờ hệ thống (Gửi 6 bytes: YY MM DD HH MM SS)
#define VP_SYS_BUZZER          0x00A0  // Điều khiển còi (Gửi 0x0040 để bíp)

// --- 2. DASHBOARD - OUTPUT DC (0x1000) ---
#define VP_DC_VOLTAGE          0x1000  // 16-bit Int (1 Decimal, VD: 521 = 52.1V)
#define VP_DC_CURRENT          0x1001  // 16-bit Int (1 Decimal, VD: 105 = 10.5A)
#define VP_DC_POWER            0x1002  // 16-bit Int (0 Decimal, VD: 3000 = 3000W)

// --- 3. DASHBOARD - BATTERY (0x1010) ---
#define VP_BAT_PACK_VOLT       0x1010  // 16-bit Int (1 Decimal)
#define VP_BAT_CELL_VOLT       0x1011  // 16-bit Int (2 Decimals, VD: 325 = 3.25V)
#define VP_BAT_CHARGED_AH      0x1012  // 32-bit Int (Tốn 2 VP: 1012 & 1013) (1 Decimal)

// --- 4. DASHBOARD - INPUT AC (0x1020) ---
#define VP_AC_PHASE_L1         0x1020  // 16-bit Int
#define VP_AC_PHASE_L2         0x1021  // 16-bit Int
#define VP_AC_PHASE_L3         0x1022  // 16-bit Int

// --- 5. DASHBOARD - TEMPERATURE (0x1030) ---
#define VP_TEMP_BATTERY        0x1030  // 16-bit Signed
#define VP_TEMP_CHARGE         0x1031  // 16-bit Signed
#define VP_TEMP_JACK           0x1032  // 16-bit Signed

// --- 6. DASHBOARD - CENTER STATUS & BUTTON (0x1040) ---
#define VP_SOC_VALUE           0x1040  // 16-bit Int (0 - 100%)
#define VP_SYS_STATUS_ICON     0x1041  // 16-bit Int (0:READY, 1:STARTING, 2:CHARGING, 3:COMPLETE, 4:ERROR, 5:OFFLINE)
#define VP_SYS_ACTION_BTN      0x1042  // 16-bit Int (Return Key Code: 1:START, 2:STOP. MCU đọc xong set về 0)

// --- 7. SETTING SCREEN (0x1100) ---
#define VP_SET_HW_VER          0x1100  // Chuỗi ASCII (Tốn 8 VP = 16 Ký tự)
#define VP_SET_FW_VER          0x1108  // Chuỗi ASCII (Tốn 8 VP = 16 Ký tự)
#define VP_SET_DEVICE_ID       0x1110  // Chuỗi ASCII (Tốn 8 VP = 16 Ký tự)
#define VP_SET_UPTIME          0x1118  // 32-bit Int (Tốn 2 VP: 1118 & 1119)

// --- 8. ALARM TABLE (0x1200) ---
/* Cấu trúc 1 dòng (20 VP):
 * - Time: +0x00 (4 VP / 8 ký tự)
 * - Level: +0x04 (1 VP / Icon ID)
 * - Code: +0x05 (4 VP / 8 ký tự)
 * - Desc: +0x09 (11 VP / 22 ký tự)
 */
#define VP_ALARM_ROW_1         0x1200
#define VP_ALARM_ROW_2         0x1220
#define VP_ALARM_ROW_3         0x1240
#define VP_ALARM_ROW_4         0x1260
#define VP_ALARM_ROW_5         0x1280

#endif // DWIN_VP_MAP_H
