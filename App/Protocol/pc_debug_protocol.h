/**
 * @file pc_debug_protocol.h
 * @brief PC Debug Protocol - Structured data for debugging charger modules
 * @note Protocol: Binary frame [0xAA][0x55][CMD][LEN][PAYLOAD][CRC8]
 */

#ifndef PC_DEBUG_PROTOCOL_H
#define PC_DEBUG_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

/* ============== Debug Commands (PC -> MCU) ============== */
#define DEBUG_CMD_ENTER           0x10    /**< Enter debug mode (no auto stream) */
#define DEBUG_CMD_EXIT            0x11    /**< Exit debug mode */
#define DEBUG_CMD_READ_ALL        0x12    /**< Read all modules (one-shot, PC poll) */
#define DEBUG_CMD_READ_ONE        0x13    /**< Read specific module by index */
#define DEBUG_CMD_READ_STATS      0x14    /**< Read communication statistics */
#define DEBUG_CMD_WRITE_REG       0x15    /**< Write to module register */
#define DEBUG_CMD_SEND_RAW_CAN    0x16    /**< Send raw CAN frame */
#define DEBUG_CMD_READ_BMS        0x17    /**< Read BMS detailed data */
#define DEBUG_CMD_GET_SYSTEM      0x18    /**< Read system information */
#define DEBUG_CMD_GET_CHARGE_CFG  0x19    /**< Read charge-cycle configuration */
#define DEBUG_CMD_SET_CHARGE_CFG  0x1A    /**< Write charge-cycle configuration */

/* ============== Debug Responses (MCU -> PC) ============== */
#define DEBUG_RSP_MODULE_DATA     0x90    /**< Single module data */
#define DEBUG_RSP_ALL_MODULES     0x91    /**< All modules data (streaming) */
#define DEBUG_RSP_COMM_STATS      0x92    /**< Communication statistics */
#define DEBUG_RSP_BMS_DATA        0x93    /**< BMS detailed data */
#define DEBUG_RSP_SYSTEM_INFO     0x94    /**< System information */
#define DEBUG_RSP_RAW_CAN_TX      0x95    /**< Raw CAN TX confirmation */
#define DEBUG_RSP_ERROR           0x96    /**< Error response */
#define DEBUG_RSP_CHARGE_CFG      0x97    /**< Charge-cycle configuration */

/* ============== Constants ============== */
#define DEBUG_MAX_MODULES         8
#define DEBUG_STREAM_INTERVAL_MS  1000

/* ============== Data Structures ============== */

/**
 * @brief Module data for debug (matches all drivers)
 * @note  Extended to include AC phases, PFC bus voltages, rated specs
 *         Total size: 126 bytes
 */
typedef struct __attribute__((packed)) {
    /* Identity (6 bytes) */
    uint8_t  module_idx;
    uint8_t  driver_id;
    uint8_t  enabled;
    uint8_t  online;
    uint8_t  running;
    uint8_t  state;

    /* Output (20 bytes) */
    float    voltage;
    float    current;
    float    current_limit;

    /* Temperatures (16 bytes) */
    float    temp_dcdc;
    float    temp_ambient;
    float    temp_pfc;           /**< PFC stage temperature (Maxwell) */

    /* AC Input 3-phase (12 bytes) */
    float    ac_phase_a_voltage;  /**< AC phase A voltage (Maxwell/TonHe) */
    float    ac_phase_b_voltage;  /**< AC phase B voltage (Maxwell/TonHe) */
    float    ac_phase_c_voltage;  /**< AC phase C voltage (Maxwell/TonHe) */

    /* PFC Bus (8 bytes) */
    float    pfc_bus_pos_voltage; /**< Positive DC bus voltage (Maxwell) */
    float    pfc_bus_neg_voltage; /**< Negative DC bus voltage (Maxwell) */

    /* Power & Ratings (12 bytes) */
    uint32_t input_power;
    float    rated_power;         /**< Rated output power (Maxwell) */
    float    rated_current;      /**< Rated output current (Maxwell) */

    /* Alarms (5 bytes) */
    uint32_t alarm_status;        /**< Raw alarm bits from module */
    uint32_t alarm_flags;        /**< Standardized alarm flags */
    uint8_t  pfc_fault;          /**< PFC fault byte (TonHe M_C_1 byte 8) */

    /* Address (2 bytes) */
    uint8_t  addr;
    uint8_t  group;

    /* Timing (8 bytes) */
    uint32_t last_rx_tick;
    uint32_t last_tx_tick;

    /* Communication Stats (20 bytes) */
    uint32_t tx_count;
    uint32_t rx_count;
    uint32_t error_count;
    uint32_t timeout_count;
    uint32_t recovery_count;

    /* Vendor extension (22 bytes) */
    uint8_t  vendor_data_len;
    uint8_t  vendor_data[21];    /**< Vendor-specific data (22 bytes) */
} DebugModuleData_t;

/** Compile-time size assertion */
_Static_assert(sizeof(DebugModuleData_t) == 123, "DebugModuleData_t must be 123 bytes");

/**
 * @brief System information response
 */
typedef struct __attribute__((packed)) {
    uint8_t  fw_major;
    uint8_t  fw_minor;
    uint8_t  fw_patch;
    uint8_t  driver_id;
    uint8_t  modules_total;
    uint8_t  modules_online;
    uint8_t  modules_fault;
    uint8_t  charging;
    uint8_t  controller_state;
    uint8_t  controller_derating;
    uint8_t  controller_inhibit;
    uint8_t  charge_source_mode;
    uint8_t  active_limit_source;
    uint8_t  active_stage_band;

    float    total_voltage;
    float    total_current;
    float    total_power_in;
    float    max_temp_dcdc;
    float    controller_target_voltage;
    float    controller_target_current_total;
    float    active_limit_current_c;

    uint32_t uptime_ticks;
    uint32_t can1_tx_count;
    uint32_t can1_rx_count;
    uint32_t can2_tx_count;
    uint32_t can2_rx_count;

    /* Controller diagnostics (appended to preserve the existing prefix) */
    uint32_t controller_fault_flags;
    uint8_t  controller_stop_reason;
    uint8_t  bms_stale;
} DebugSystemInfo_t;

_Static_assert(sizeof(DebugSystemInfo_t) == 68, "DebugSystemInfo_t must be 68 bytes");

/* ============== Function Declarations ============== */

/**
 * @brief Initialize debug protocol
 */
void DebugProtocol_Init(void);

/**
 * @brief Enter debug mode, start streaming
 */
void DebugProtocol_Enter(void);

/**
 * @brief Exit debug mode, stop streaming
 */
void DebugProtocol_Exit(void);

/**
 * @brief Check if debug mode is active
 */
bool DebugProtocol_IsActive(void);

/**
 * @brief Process debug command from PC
 * @param cmd Command code
 * @param payload Command payload
 * @param len Payload length
 * @return true if command handled
 */
bool DebugProtocol_HandleCommand(uint8_t cmd, const uint8_t *payload, uint16_t len);

/**
 * @brief Send all modules data (streaming callback)
 * @note Call this from main loop when debug mode is active
 */
void DebugProtocol_SendStream(void);

/**
 * @brief Build single module data
 * @param idx Module index
 * @param data Output buffer
 * @return Bytes written, 0 if invalid
 */
uint16_t DebugProtocol_BuildModuleData(uint8_t idx, uint8_t *data);

/**
 * @brief Build all modules data
 * @param data Output buffer
 * @param max_len Max buffer size
 * @return Bytes written
 */
uint16_t DebugProtocol_BuildAllModulesData(uint8_t *data, uint16_t max_len);

/**
 * @brief Build BMS data
 * @param data Output buffer
 * @param max_len Max buffer size
 * @return Bytes written
 */
uint16_t DebugProtocol_BuildBMSData(uint8_t *data, uint16_t max_len);

/**
 * @brief Build system info
 * @param data Output buffer
 * @param max_len Max buffer size
 * @return Bytes written
 */
uint16_t DebugProtocol_BuildSystemInfo(uint8_t *data, uint16_t max_len);

/**
 * @brief Build comm stats for module
 * @param idx Module index
 * @param data Output buffer
 * @return Bytes written
 */
uint16_t DebugProtocol_BuildCommStats(uint8_t idx, uint8_t *data);

/**
 * @brief Build charge-cycle configuration snapshot
 * @param data Output buffer
 * @param max_len Max buffer size
 * @return Bytes written
 */
uint16_t DebugProtocol_BuildChargeConfig(uint8_t *data, uint16_t max_len);

#endif /* PC_DEBUG_PROTOCOL_H */

