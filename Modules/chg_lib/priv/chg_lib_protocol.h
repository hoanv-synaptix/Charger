/**
 * @file chg_lib_protocol.h
 * @brief   Canonical Charger Protocol - Common types and helpers
 * @note    Shared data conversion utilities for all charger drivers.
 *          Each driver (Maxwell/Lianming/TonHe) implements its own CAN ID builder.
 *
 * Architecture:
 *   - Each driver is independent and implements CHG_LIB_DriverOps_t interface
 *   - CAN ID builders are driver-specific (no shared implementation)
 *   - Data format converters are shared (Float/U32/U16 helpers)
 *
 * Historical Note:
 *   - CHG_ProtocolBuildCanId was removed as each module uses different CAN ID formats:
 *     - Maxwell: Proprietary 29-bit format (PROTNO + PTP + DST + SRC + Group)
 *     - Lianming: Custom Command ID format (Command + Module Address)
 *     - TonHe: J1939 format (Priority + PF + PS + SA)
 *
 * Dependencies:
 *   - driver_maxwell.c: Uses Float helpers (IEEE 754)
 *   - driver_lianming.c: Uses U16 helpers (0.1V, 0.01A)
 *   - driver_tonhe.c: Uses its own conversion (0.1V, 0.01A)
 */

#ifndef CHG_LIB_PROTOCOL_H
#define CHG_LIB_PROTOCOL_H

#include <stdint.h>

/* =========================================================
 * CANONICAL CHARGER REGISTERS
 * Standard register map dùng chung cho tất cả module
 * (Định nghĩa theo Maxwell, các module khác map tương ứng)
 * ========================================================= */
#define CHG_LIB_REG_VOLTAGE                 0x0001  /* Đọc điện áp output */
#define CHG_LIB_REG_CURRENT                0x0002  /* Đọc dòng output */
#define CHG_LIB_REG_CURR_LIMIT             0x0003  /* Đọc điểm giới hạn dòng */
#define CHG_LIB_REG_TEMP_DCDC              0x0004  /* Đọc nhiệt độ board DCDC */
#define CHG_LIB_REG_INPUT_DC_VOLTAGE       0x0005  /* Đọc điện áp DC đầu vào (hoặc input voltage) */
#define CHG_LIB_REG_PFC0_VOLTAGE           0x0008  /* Đọc điện áp PFC Bus dương */
#define CHG_LIB_REG_PFC1_VOLTAGE           0x000A  /* Đọc điện áp PFC Bus âm */
#define CHG_LIB_REG_TEMP_AMBIENT           0x000B  /* Đọc nhiệt độ môi trường */
#define CHG_LIB_REG_AC_PHASE_A             0x000C  /* Đọc điện áp AC pha A */
#define CHG_LIB_REG_AC_PHASE_B             0x000D  /* Đọc điện áp AC pha B */
#define CHG_LIB_REG_AC_PHASE_C             0x000E  /* Đọc điện áp AC pha C */
#define CHG_LIB_REG_TEMP_PFC               0x0010  /* Đọc nhiệt độ board PFC */
#define CHG_LIB_REG_RATED_POWER            0x0011  /* Công suất định mức */
#define CHG_LIB_REG_RATED_CURRENT          0x0012  /* Dòng định mức */
#define CHG_LIB_REG_SET_POWER              0x0020  /* Cài đặt công suất */
#define CHG_LIB_REG_SET_VOLTAGE            0x0021  /* Cài đặt điện áp */
#define CHG_LIB_REG_SET_CURR_LIMIT         0x0022  /* Cài đặt giới hạn dòng */
#define CHG_LIB_REG_SET_OVP                0x0023  /* Cài đặt OVP */
#define CHG_LIB_REG_ON_OFF                 0x0030  /* Bật/Tắt module */
#define CHG_LIB_REG_ALARM_STATUS           0x0040  /* Trạng thái alarm */
#define CHG_LIB_REG_GROUP_ADDR             0x0043  /* Địa chỉ group */
#define CHG_LIB_REG_SHORT_RESET            0x0044  /* Reset lỗi short */
#define CHG_LIB_REG_INPUT_MODE_SET         0x0046  /* Cài đặt chế độ input */
#define CHG_LIB_REG_INPUT_POWER            0x0048  /* Công suất input */
#define CHG_LIB_REG_INPUT_MODE_RD          0x004B  /* Đọc chế độ input */

/* An toàn: giới hạn công suất đầu vào hợp lý */
#define CHG_LIB_INPUT_POWER_MAX_W           100000U

/* =========================================================
 * DATA CONVERSION HELPERS
 * ========================================================= */

#include <string.h>

/**
 * @brief  Float to Big-Endian (IEEE 754)
 * @note   Dùng cho Maxwell (Float format)
 */
static inline void CHG_LIB_ProtocolFloatToBE(float value, uint8_t *out)
{
    uint32_t u;
    memcpy(&u, &value, sizeof(u));
    out[0] = (uint8_t)(u >> 24);
    out[1] = (uint8_t)(u >> 16);
    out[2] = (uint8_t)(u >> 8);
    out[3] = (uint8_t)(u);
}

/**
 * @brief  Big-Endian to Float (IEEE 754)
 * @note   Dùng cho Maxwell (Float format)
 */
static inline float CHG_LIB_ProtocolBEToFloat(const uint8_t *in)
{
    uint32_t u = ((uint32_t)in[0] << 24) |
                 ((uint32_t)in[1] << 16) |
                 ((uint32_t)in[2] << 8) |
                 ((uint32_t)in[3]);
    float f;
    memcpy(&f, &u, sizeof(f));
    return f;
}

/**
 * @brief  UInt32 to Big-Endian
 */
static inline void CHG_LIB_ProtocolU32ToBE(uint32_t value, uint8_t *out)
{
    out[0] = (uint8_t)(value >> 24);
    out[1] = (uint8_t)(value >> 16);
    out[2] = (uint8_t)(value >> 8);
    out[3] = (uint8_t)value;
}

/**
 * @brief  Big-Endian to UInt32
 */
static inline uint32_t CHG_LIB_ProtocolBEToU32(const uint8_t *in)
{
    return ((uint32_t)in[0] << 24) |
           ((uint32_t)in[1] << 16) |
           ((uint32_t)in[2] << 8) |
           ((uint32_t)in[3]);
}

/**
 * @brief  UInt16 to Big-Endian (dùng cho Lianming - 2 byte data)
 */
static inline void CHG_LIB_ProtocolU16ToBE(uint16_t value, uint8_t *out)
{
    out[0] = (uint8_t)(value >> 8);
    out[1] = (uint8_t)value;
}

/**
 * @brief  Big-Endian to UInt16
 */
static inline uint16_t CHG_LIB_ProtocolBEToU16(const uint8_t *in)
{
    return ((uint16_t)in[0] << 8) | ((uint16_t)in[1]);
}

#endif /* CHARGER_PROTOCOL_H */
