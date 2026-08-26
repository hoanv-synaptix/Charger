/**
 * @file    pc_protocol.h
 * @brief   Binary protocol PC ↔ STM32 qua USB CDC
 * @note    Frame: [0xAA][0x55][CMD][LEN][PAYLOAD...][CRC8]
 *          CRC8 poly=0x07, tính trên [CMD, LEN, PAYLOAD]
 *          Payload: little-endian
 */

#ifndef PC_PROTOCOL_H
#define PC_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

/* Frame constants */
#define PC_SOF1             0xAA
#define PC_SOF2             0x55
#define PC_MAX_PAYLOAD      255
#define PC_CRC8_POLY        0x07

/* Commands PC -> STM32 */
#define PC_CMD_SET_VOLTAGE      0x01
#define PC_CMD_SET_CURRENT      0x02
#define PC_CMD_START            0x03
#define PC_CMD_STOP             0x04
#define PC_CMD_SET_MODULE_ADDR  0x05
#define PC_CMD_PING             0x06
#define PC_CMD_READ_REG         0x07
#define PC_CMD_EMERGENCY_STOP   0x08
#define PC_CMD_SET_DRIVER       0x09

/* Responses STM32 -> PC */
#define PC_RSP_STATUS           0x81
#define PC_RSP_ACK              0x82
#define PC_RSP_NACK             0x83
#define PC_RSP_PONG             0x84
#define PC_RSP_READ_REG         0x85

/* NACK error codes */
#define PC_ERR_BAD_CRC          0x01
#define PC_ERR_UNKNOWN_CMD      0x02
#define PC_ERR_BAD_LENGTH       0x03
#define PC_ERR_CAN_TX_FAIL      0x04
#define PC_ERR_BAD_PARAM        0x05

/* Status report (gửi định kỳ về PC) */
#pragma pack(push, 1)
typedef struct {
    /* Charger Info (27 bytes) */
    float    voltage;        /* V đầu ra (chung, từ module đầu tiên) */
    float    total_current;  /* Tổng A từ tất cả module */
    float    temp_dcdc;      /* Nhiệt độ DCDC cao nhất */
    float    temp_ambient;   /* Nhiệt độ môi trường */
    uint32_t alarm_status;   /* Mapped CHG_AlarmFlag_t */
    uint32_t total_power_in; /* Tổng W input */
    uint8_t  modules_online; /* Số module đang chạy */
    uint8_t  modules_fault;  /* Số module lỗi */
    uint8_t  charging;       /* 1 = có module đang sạc */
    
    /* BMS Info (22 bytes) */
    float    bms_voltage;    
    float    bms_current;    
    float    bms_chg_v_req;  
    float    bms_chg_i_req;  
    uint32_t bms_alarm;      /* Mapped BMS_AlarmFlag_t */
    uint8_t  bms_soc;        
    uint8_t  bms_state;      
    
    /* System (2 bytes) */
    uint8_t  btn_start;      /* Trạng thái nút Start */
    uint8_t  btn_stop;       /* Trạng thái nút Stop */
} PC_StatusReport_t;        /* 51 bytes */

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(sizeof(PC_StatusReport_t) == 51, "PC_StatusReport_t must stay 51 bytes");
#endif

#pragma pack(pop)

/* ============== API ============== */

/** Nạp 1 byte từ USB CDC RX vào parser */
void PC_Protocol_FeedByte(uint8_t byte);

/** Gửi status report về PC (gọi định kỳ) */
void PC_Protocol_SendStatus(void);

/** Gửi PONG response */
void PC_Protocol_SendPong(void);

/** Gửi raw frame với cmd tùy ý (dùng bởi debug protocol) */
void PC_Protocol_SendFrame(uint8_t cmd, const uint8_t *payload, uint16_t len);

/** Process one queued USB TX frame when CDC is ready. */
void PC_Protocol_ProcessTx(void);

/** Notify the protocol layer that the USB TX transfer completed. */
void PC_Protocol_NotifyTxComplete(void);

/** Reset queued/in-flight debug TX state when a new USB debug session starts. */
void PC_Protocol_ResetTx(void);

/** Kiểm tra trạng thái charging (do PC đặt) */
bool PC_Protocol_IsCharging(void);

/** Firmware version */
#define FW_VERSION_MAJOR    2
#define FW_VERSION_MINOR    0
#define FW_VERSION_PATCH    0

#endif /* PC_PROTOCOL_H */


