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
#define PC_CMD_RESET_FAULT      0x0A
#define PC_CMD_SET_OTA_POLICY   0x0B  /* payload: enabled,u32 interval,HTTPS manifest URL */
#define PC_CMD_GET_OTA_STATUS   0x0C
#define PC_CMD_OTA_CHECK_NOW    0x0D
#define PC_CMD_OTA_APPLY        0x0E
#define PC_CMD_TEST_FLASH       0x0F
#define PC_CMD_TEST_SD          0x20
#define PC_CMD_OTA_UPLOAD_START 0x21  /* payload: u32 size, u32 crc32, u32 version */
#define PC_CMD_OTA_UPLOAD_CHUNK 0x22  /* payload: u32 offset, u8 data[...] */
#define PC_CMD_OTA_UPLOAD_FINISH 0x23 /* payload: none -> verify & arm bootloader */
#define PC_CMD_GET_4G_STATUS    0x24 /* payload: none -> returns 4G modem & network status */
#define PC_CMD_ERASE_FLASH      0x25 /* payload: u32 admin_pin, u8 erase_mask */

/* Bitmask definitions for PC_CMD_ERASE_FLASH */
#define ERASE_MASK_CONFIG       0x01U /* Reset ChargeCycleConfig to factory defaults */
#define ERASE_MASK_ALARM_LOG    0x02U /* Erase persistent alarm history */
#define ERASE_MASK_ENERGY       0x04U /* Reset persistent accumulated energy (kWh/Ah) */
#define ERASE_MASK_ALL          (ERASE_MASK_CONFIG | ERASE_MASK_ALARM_LOG | ERASE_MASK_ENERGY)

/* Responses STM32 -> PC */
#define PC_RSP_STATUS           0x81
#define PC_RSP_ACK              0x82
#define PC_RSP_NACK             0x83
#define PC_RSP_PONG             0x84
#define PC_RSP_READ_REG         0x85
#define PC_RSP_OTA_STATUS       0x86
#define PC_RSP_FLASH_TEST       0x87
#define PC_RSP_SD_TEST          0x88
#define PC_RSP_4G_STATUS        0x89

/* NACK error codes */
#define PC_ERR_BAD_CRC          0x01
#define PC_ERR_UNKNOWN_CMD      0x02
#define PC_ERR_BAD_LENGTH       0x03
#define PC_ERR_CAN_TX_FAIL      0x04
#define PC_ERR_BAD_PARAM        0x05

/* OTA-specific NACK error codes */
#define PC_ERR_OTA_BUSY             0x20  /* OTA service currently running or busy */
#define PC_ERR_OTA_NOT_SAFE         0x21  /* Safety interlock active (charging, fault, emergency stop) */
#define PC_ERR_OTA_NET_NOT_READY    0x22  /* 4G modem or PDP context not ready */
#define PC_ERR_OTA_FLASH_BUSY       0x23  /* External SPI flash unavailable or busy */
#define PC_ERR_OTA_POLICY_DISABLED  0x24  /* OTA policy not enabled or URL empty */

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
    uint32_t bms_alarm;      /* Reported BMS flags: warning + fault */
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

/** Process complete frames queued by the USB RX parser in main-loop context. */
void PC_Protocol_ProcessRx(void);

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

/** Number of frames currently queued for TX (0..PC_TX_QUEUE_DEPTH). Test/
 *  debug support -- lets a host test (or a future debug command) inspect
 *  what the protocol layer actually enqueued, without needing a real USB
 *  transfer. */
uint8_t PC_Protocol_GetTxQueueDepth(void);

/** Peek at a queued TX frame without consuming it. index 0 = oldest frame
 *  (the next one PC_Protocol_ProcessTx() would send). Writes the frame's
 *  cmd byte and payload into the caller's buffers (payload must be at
 *  least PC_MAX_PAYLOAD bytes) and returns true, or returns false if
 *  index is beyond the current queue depth. Test/debug support -- read-only,
 *  does not affect PC_Protocol_ProcessTx()'s send order. */
bool PC_Protocol_PeekTxFrame(uint8_t index, uint8_t *cmd, uint8_t *payload, uint8_t *payload_len);

/** Firmware and hardware version */
#include "app_version.h"

#endif /* PC_PROTOCOL_H */

