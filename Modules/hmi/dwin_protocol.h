#ifndef DWIN_PROTOCOL_H
#define DWIN_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

/* DWIN Header & Commands */
#define DWIN_HEADER_1 0x5A
#define DWIN_HEADER_2 0xA5
#define DWIN_WRITE    0x82
#define DWIN_READ     0x83

/* VP Addresses (Từ dwin_vp_map.md) */
#define VP_SYS_STATUS   0x1000
#define VP_CMD_CTRL     0x1002
#define VP_DC_VOLT      0x1100
#define VP_DC_CURR      0x1102
#define VP_CHARGE_TIME  0x1110 // Chuỗi String (Nhiều Words)
#define VP_BAT_SOC      0x1200
#define VP_BAT_PACK_V   0x1202
#define VP_BAT_CELL_V   0x1204
#define VP_AC_L1        0x1300
#define VP_AC_L2        0x1302
#define VP_AC_L3        0x1304
#define VP_TEMP_CHARGER 0x1400
#define VP_TEMP_BAT     0x1402
#define VP_FAULT_CODE   0x2000

/* Fault Codes (Tương ứng với Bit Variable Icon ID trên DWIN 0.ICO) */
typedef enum {
    FAULT_NONE            = 0,
    FAULT_BMS_OFFLINE     = 1,
    FAULT_OVER_VOLT       = 2,
    FAULT_OVER_CURR       = 3,
    FAULT_OVER_TEMP       = 4,
    FAULT_CHARGER_OFFLINE = 5,
    FAULT_AC_INPUT        = 6,
    FAULT_HARDWARE        = 7
} DWIN_FaultCode_e;

/* Cấu trúc dữ liệu Hệ thống để gửi lên DWIN */
typedef struct {
    uint16_t dc_volt_x10;
    uint16_t dc_curr_x10;
    uint16_t bat_soc;
    uint16_t bat_pack_v_x10;
    uint16_t bat_cell_v_x100;
    uint16_t ac_l1;
    uint16_t ac_l2;
    uint16_t ac_l3;
    uint16_t temp_charger;
    uint16_t temp_bat;
    uint16_t sys_status; // 0=Standby, 1=Run, 2=Fault
    uint16_t fault_code;
} DWIN_SystemData_t;

/* Khởi tạo (Nếu cần thiết lập UART DMA/IT) */
void DWIN_Init(void);

/* Cập nhật toàn bộ giao diện */
void DWIN_UpdateData(const DWIN_SystemData_t* data);

/* Các hàm truyền cơ bản */
void DWIN_SendInt(uint16_t vp_addr, uint16_t value);
void DWIN_SendString(uint16_t vp_addr, const char* str);

/* Hàm parse dữ liệu nhận (Đặt trong ngắt UART hoặc vòng lặp Rx) */
void DWIN_ParseRX(uint8_t* buffer, uint16_t len);

/* Callback được gọi khi có lệnh từ DWIN (Nút nhấn Start/Stop) */
void DWIN_OnCommandReceived(uint16_t command);

#endif // DWIN_PROTOCOL_H
