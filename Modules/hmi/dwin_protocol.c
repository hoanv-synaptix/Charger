#include "dwin_protocol.h"
#include <string.h>

/* Yêu cầu người dùng (User) định nghĩa hàm này ở file main.c hoặc uart.c 
   để gửi mảng byte qua ngoại vi UART kết nối với DWIN */
extern void UART_Transmit_To_DWIN(uint8_t* data, uint16_t len);

void DWIN_Init(void) {
    // Khởi tạo các biến nếu cần thiết
}

/* Hàm gửi 1 Word (2 bytes) dữ liệu dạng số nguyên lên địa chỉ VP */
void DWIN_SendInt(uint16_t vp_addr, uint16_t value) {
    uint8_t tx_buf[8];
    tx_buf[0] = DWIN_HEADER_1;
    tx_buf[1] = DWIN_HEADER_2;
    tx_buf[2] = 0x05; // Độ dài payload: Command(1) + Address(2) + Data(2)
    tx_buf[3] = DWIN_WRITE;
    tx_buf[4] = (uint8_t)(vp_addr >> 8);
    tx_buf[5] = (uint8_t)(vp_addr & 0xFF);
    tx_buf[6] = (uint8_t)(value >> 8);
    tx_buf[7] = (uint8_t)(value & 0xFF);
    
    UART_Transmit_To_DWIN(tx_buf, 8);
}

/* Hàm gửi chuỗi String lên địa chỉ VP */
void DWIN_SendString(uint16_t vp_addr, const char* str) {
    uint16_t len = strlen(str);
    if (len > 32) len = 32; // Giới hạn chống tràn bộ đệm
    
    uint8_t tx_buf[64];
    tx_buf[0] = DWIN_HEADER_1;
    tx_buf[1] = DWIN_HEADER_2;
    tx_buf[2] = 3 + len; // Command(1) + Address(2) + Data Length
    tx_buf[3] = DWIN_WRITE;
    tx_buf[4] = (uint8_t)(vp_addr >> 8);
    tx_buf[5] = (uint8_t)(vp_addr & 0xFF);
    
    memcpy(&tx_buf[6], str, len);
    
    UART_Transmit_To_DWIN(tx_buf, 6 + len);
}

/* Hàm cập nhật toàn bộ các tham số UI lên màn hình (Scatter) */
void DWIN_UpdateData(const DWIN_SystemData_t* data) {
    static uint8_t state = 0;
    switch (state) {
        case 0: DWIN_SendInt(VP_SYS_STATUS, data->sys_status); break;
        case 1: DWIN_SendInt(VP_DC_VOLT, data->dc_volt_x10); break;
        case 2: DWIN_SendInt(VP_DC_CURR, data->dc_curr_x10); break;
        case 3: DWIN_SendInt(VP_BAT_SOC, data->bat_soc); break;
        case 4: DWIN_SendInt(VP_BAT_PACK_V, data->bat_pack_v_x10); break;
        case 5: DWIN_SendInt(VP_BAT_CELL_V, data->bat_cell_v_x100); break;
        case 6: DWIN_SendInt(VP_AC_L1, data->ac_l1); break;
        case 7: DWIN_SendInt(VP_AC_L2, data->ac_l2); break;
        case 8: DWIN_SendInt(VP_AC_L3, data->ac_l3); break;
        case 9: DWIN_SendInt(VP_TEMP_CHARGER, data->temp_charger); break;
        case 10: DWIN_SendInt(VP_TEMP_BAT, data->temp_bat); break;
        case 11: DWIN_SendInt(VP_FAULT_CODE, data->fault_code); break;
        default: state = 0; return; /* Reset and return */
    }
    state++;
    if (state > 11) state = 0;
}

/* Hàm phân tích dữ liệu nhận về từ DWIN (Khi có sự kiện nút bấm) */
void DWIN_ParseRX(uint8_t* buffer, uint16_t len) {
    static uint8_t rx_buf[64];
    static uint8_t rx_idx = 0;
    static uint8_t expected_len = 0;
    
    for (uint16_t i = 0; i < len; i++) {
        uint8_t b = buffer[i];
        if (rx_idx == 0) {
            if (b == DWIN_HEADER_1) rx_buf[rx_idx++] = b;
        } else if (rx_idx == 1) {
            if (b == DWIN_HEADER_2) rx_buf[rx_idx++] = b;
            else { rx_idx = 0; if (b == DWIN_HEADER_1) rx_buf[rx_idx++] = b; }
        } else if (rx_idx == 2) {
            expected_len = b;
            rx_buf[rx_idx++] = b;
            if (expected_len == 0 || expected_len > 60) rx_idx = 0; // Invalid length
        } else {
            rx_buf[rx_idx++] = b;
            if (rx_idx >= expected_len + 3) {
                // Đủ frame
                if (rx_buf[3] == DWIN_READ) {
                    uint16_t vp_addr = (rx_buf[4] << 8) | rx_buf[5];
                    uint8_t data_words = rx_buf[6];
                    if (vp_addr == VP_CMD_CTRL && data_words == 1 && rx_idx >= 9) {
                        uint16_t cmd_val = (rx_buf[7] << 8) | rx_buf[8];
                        DWIN_OnCommandReceived(cmd_val);
                    }
                }
                rx_idx = 0;
            }
        }
    }
}

/* Hàm yếu (Weak callback). Người dùng cần viết đè (override) lại ở main.c */
__attribute__((weak)) void DWIN_OnCommandReceived(uint16_t command) {
    // Ví dụ:
    // if (command == 1) { /* Start Charger */ }
    // else if (command == 2) { /* Stop Charger */ }
}
