/**
 * @file    bms_protocol.h
 * @brief   BMS CAN Protocol - Message IDs, Data Types, and Parsing
 * @note    Protocol: CAN 2.0A/B, 250Kbps, Little-Endian
 *
 * Physical Interface:
 *   - CAN2: 250Kbps. Current CubeMX timing is Prescaler=12,
 *     BS1=11TQ, BS2=2TQ, SJW=1TQ on a 42MHz CAN clock.
 *   - CAN2 GPIO: PB12=RX, PB13=TX
 *
 * Message Overview:
 *   Standard frames (11-bit): BATT_ST1, CELL_VOLT, CELL_TEMP, ALM_INFO
 *   Extended frames (29-bit): BATT_ST2, ChgRequest, Ctrl_INFO, BmsSwSta,
 *                              CELL_VOLT_FULL (8 frames), CELL_TEMP_FULL
 *
 * CAN ID Layout (Standard):
 *   - 0x02F4: BATT_ST1    (BMS → Charger, 20ms)
 *   - 0x04F4: CELL_VOLT   (BMS → Charger, 100ms)
 *   - 0x05F4: CELL_TEMP   (BMS → Charger, 500ms)
 *   - 0x07F4: ALM_INFO    (BMS → Charger, event-triggered)
 *
 * CAN ID Layout (Extended 29-bit):
 *   - 0x18F128F4: BATT_ST2         (BMS → Charger, 100ms)
 *   - 0x18F0F428: Ctrl_INFO        (Charger → BMS, on-demand)
 *   - 0x1806E5F4: ChgRequest_INFO  (BMS → Charger, 1000ms)
 *   - 0x18F528F4: BmsSwSta         (BMS → Charger, 500ms)
 *   - 0x18E028F4..0x18E728F4: CELL_VOLT_FULL (BMS → Charger, 1000ms)
 *   - 0x18F228F4: CELL_TEMP_FULL   (BMS → Charger, 1000ms)
 *
 * References:
 *   - "CAN BMS_BB_PKG V1.0.pdf"
 *
 * Vendor document notes:
 *   - BmsSwSta lists start position 0 for all three switch fields. This is
 *     ambiguous, so this driver maps byte0 bit0/bit1/bit2 to
 *     pre-discharge/discharge/charge until real CAN logs prove otherwise.
 *   - The overview table shows 0x1806E5F4 next to ChgRequest_INFO, but the
 *     detailed message section only defines 0x1806E5F4. Keep 0x1806E5F4 as
 *     the active ID unless a real BMS log shows the alternate ID.
 */

#ifndef BMS_PROTOCOL_H
#define BMS_PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============== CAN Message IDs ============== */

/** Standard Frame IDs (11-bit) */
#define BMS_ID_BATT_ST1   0x02F4U
#define BMS_ID_CELL_VOLT  0x04F4U
#define BMS_ID_CELL_TEMP  0x05F4U
#define BMS_ID_ALM_INFO   0x07F4U

/** Extended Frame IDs (29-bit) */
#define BMS_ID_BATT_ST2          0x18F128F4UL
#define BMS_ID_CTRL_INFO         0x18F0F428UL  /* Charger → BMS */
#define BMS_ID_CHG_REQUEST       0x1806E5F4UL
#define BMS_ID_BMS_SW_STA        0x18F528F4UL
#define BMS_ID_CELL_VOLT_FULL(n) (0x18E028F4UL | (((uint32_t)(n) & 0x07U) << 16))
#define BMS_ID_CELL_VOLT_FULL_MASK 0xFFF8FFFFUL
#define BMS_ID_CELL_TEMP_FULL    0x18F228F4UL

/* Candidate ID seen only in the PDF overview table, not enabled by default. */
#define BMS_ID_CHG_REQUEST_ALT_CANDIDATE 0x1806E5F4UL

/* ============== Parse Results ============== */

/**
 * @brief   Parse result for each CAN message.
 *          All raw values are stored as-is (no scaling applied).
 *          Apply resolution/offset only when reading.
 */
typedef bool BMS_ParseResult_t;

typedef enum {
    BMS_PARSE_OK = 0,
    BMS_PARSE_REJECT_ARGUMENT,
    BMS_PARSE_REJECT_UNKNOWN_ID,
    BMS_PARSE_REJECT_DLC
} BMS_ParseRejectReason_t;

/* ---- BATT_ST1 (0x02F4) ---- */
typedef struct {
    BMS_ParseResult_t valid;
    uint16_t raw_volt;    /* Total battery voltage: raw = V × 10   (0~1000 => 0~100.0V) */
    int16_t  raw_curr;    /* Total current: raw = (I + 400) × 10  (-400~1000A) */
    uint8_t  soc;         /* SOC 0~100% */
} BMS_BattSt1_t;

/* ---- CELL_VOLT (0x04F4) ---- */
typedef struct {
    BMS_ParseResult_t valid;
    uint16_t max_cell_volt;  /* mV, max cell voltage */
    uint8_t  max_cv_no;      /* 1-based position of max cell */
    uint16_t min_cell_volt;  /* mV, min cell voltage */
    uint8_t  min_cv_no;     /* 1-based position of min cell */
} BMS_CellVolt_t;

/* ---- CELL_TEMP (0x05F4) ---- */
typedef struct {
    BMS_ParseResult_t valid;
    uint8_t max_cell_temp;  /* raw = Temp + 50 (0~250, range -50~200°C) */
    uint8_t max_ct_no;      /* 1-based position of max temp sensor */
    uint8_t min_cell_temp;  /* raw = Temp + 50 */
    uint8_t min_ct_no;      /* 1-based position of min temp sensor */
    uint8_t avg_cell_temp;  /* raw = Avg + 50 */
} BMS_CellTemp_t;

/* ---- ALM_INFO (0x07F4) ---- */
typedef struct {
    BMS_ParseResult_t valid;
    /* Each alarm: 0=none, 1..3=active fault severity (Motorola MSB-first per byte) */
    uint8_t low_pack_volt;          /* Byte 0 bits 7-6 */
    uint8_t low_cell_volt;          /* Byte 0 bits 5-4 */
    uint8_t high_pack_volt;         /* Byte 0 bits 3-2 */
    uint8_t high_cell_volt;         /* Byte 0 bits 1-0 */
    uint8_t temp_cell_high_chg;     /* Byte 1 bits 7-6 */
    uint8_t temp_cell_high_dchg;    /* Byte 1 bits 5-4 */
    uint8_t temp_cell_low_chg;      /* Byte 1 bits 3-2 */
    uint8_t temp_cell_low_dchg;     /* Byte 1 bits 1-0 */
    uint8_t temp_relay_high;        /* Byte 2 bits 7-6 */
    uint8_t over_chg_curr;          /* Byte 2 bits 5-4 */
    uint8_t over_dchg_curr;         /* Byte 2 bits 3-2 */
    uint8_t cell_volt_diff;         /* Byte 2 bits 1-0 */
    uint8_t low_soc;                /* Byte 3 bits 7-6 */
} BMS_AlmInfo_t;

/* ---- BATT_ST2 (0x18F128F4) ---- */
typedef struct {
    BMS_ParseResult_t valid;
    uint16_t cap_remain;     /* Ah × 0.1  (0~1000 => 0~100.0Ah) */
    uint16_t rate_cap;       /* Ah × 0.1  (rated capacity) */
    uint16_t cycle_count;    /* Cycle count */
    uint8_t  soh;            /* SOH 0~100% */
} BMS_BattSt2_t;

/* ---- ChgRequest_INFO (0x1806E5F4) ---- 
 * NOTE: Unlike other Jikong BMS frames, this specific message uses BIG-ENDIAN format.
 * Byte 0-1: Charging Voltage Request (Big-Endian)
 * Byte 2-3: Charging Current Request (Big-Endian)
 * Byte 4: Charger Switch (0: ON, 1: OFF) - currently ignored by parser
 * Byte 5: Charge Mode (0: Charge, 1: Heat) - currently ignored by parser
 */
typedef struct {
    BMS_ParseResult_t valid;
    uint16_t batt_volt_req;   /* V × 0.1   (0~10000 => 0~1000.0V) */
    uint16_t batt_curr_req;  /* A × 0.1   (0~10000 => 0~1000.0A) */
} BMS_ChgRequest_t;

/* ---- BmsSwSta (0x18F528F4) ---- */
typedef struct {
    BMS_ParseResult_t valid;
    /* PDF start positions are ambiguous; use byte0 bit0/1/2 until verified. */
    bool pre_discharge_sta;   /* Pre-discharge relay: 0=open, 1=closed */
    bool discharge_sta;       /* Discharge relay: 0=open, 1=closed */
    bool charge_sta;         /* Charge relay: 0=open, 1=closed */
} BMS_BmsSwSta_t;

/* ---- CELL_VOLT_FULL (0x18E028F4..0x18E728F4) ---- */
/** Each frame carries 4 cell voltages. 8 frames × 4 = 32 cells max. */
typedef struct {
    BMS_ParseResult_t valid;
    uint16_t cell[4];        /* mV, 4 cells per frame */
} BMS_CellVoltFull_t;

/* ---- CELL_TEMP_FULL (0x18F228F4) ---- */
typedef struct {
    BMS_ParseResult_t valid;
    uint8_t temp_relay;     /* raw = T + 50 (range -50~200°C) */
    uint8_t temp_shunt;     /* raw = T + 50 */
    uint8_t cell_temp[6];   /* raw = T + 50, 6 cell temperatures */
} BMS_CellTempFull_t;

/* ============== Control Info (Charger → BMS) ============== */

/**
 * @brief   Ctrl_INFO frame payload to send to BMS.
 *          ID = 0x18F0F428 (Extended)
 *
 * MaskCode bit0: Charging control  (1=allow, 0=disable)
 * MaskCode bit1: Discharge control (1=allow, 0=disable)
 * ChgSw:   0=Off, 1=On
 * DchgSw:  0=Off, 1=On
 */
typedef struct {
    uint8_t mask_code;   /* bit0=charge ctrl, bit1=discharge ctrl */
    uint8_t chg_sw;      /* 0=Off, 1=On */
    uint8_t dchg_sw;     /* 0=Off, 1=On */
    uint8_t rsv[5];      /* Reserved, send as 0 */
} BMS_CtrlInfo_t;

/* ============== Master BMS Data Container ============== */

#define BMS_MAX_CELL_VOLT_FRAMES  8U   /* 8 frames × 4 cells = 32 cells */
#define BMS_MAX_CELL_TEMP_SENSORS 8U   /* Relay, Shunt + 6 cell temps */

typedef enum {
    BMS_FRAME_BATT_ST1 = 0,
    BMS_FRAME_BATT_ST2,
    BMS_FRAME_CELL_VOLT,
    BMS_FRAME_CELL_TEMP,
    BMS_FRAME_ALM_INFO,
    BMS_FRAME_CHG_REQUEST,
    BMS_FRAME_BMS_SW_STA,
    BMS_FRAME_CELL_VOLT_FULL,
    BMS_FRAME_CELL_TEMP_FULL,
    BMS_FRAME_MAX
} BMS_FrameType_t;

typedef struct {
    BMS_BattSt1_t        batt_st1;
    BMS_CellVolt_t      cell_volt;
    BMS_CellTemp_t      cell_temp;
    BMS_AlmInfo_t       alm_info;
    BMS_BattSt2_t       batt_st2;
    BMS_ChgRequest_t    chg_request;
    BMS_BmsSwSta_t      bms_sw_sta;
    BMS_CellVoltFull_t  cell_volt_full[BMS_MAX_CELL_VOLT_FRAMES];
    BMS_CellTempFull_t  cell_temp_full;
    uint32_t            last_rx_tick[BMS_FRAME_MAX];
} BMS_Data_t;

/* ============== Convenience Accessors ============== */

/* Raw → Physical conversions */
#define BMS_RAW_TO_VOLT(raw)        (((float)(raw)) * 0.1f)
#define BMS_RAW_TO_CURR(raw)        (((float)((int16_t)(raw))) * 0.1f - 400.0f)
#define BMS_RAW_TO_TEMP(raw)        (((float)((uint8_t)(raw))) - 50.0f)
#define BMS_RAW_TO_CAP_AH(raw)      (((float)(raw)) * 0.1f)
#define BMS_RAW_TO_REQ_CURR(raw)    (((float)(raw)) * 0.1f)

/* ============== Parsing API ============== */

/**
 * @brief   Parse a raw CAN frame from BMS.
 * @note    Dispatches to the correct parser based on CAN ID.
 *          Stores result in the provided BMS_Data_t.
 * @param   ext_id     CAN extended ID (use 0 for standard frame)
 * @param   std_id     CAN standard ID (11-bit, 0 if extended)
 * @param   data       8-byte payload (little-endian)
 * @param   dlc        Data length (0-8)
 * @param   bms        Pointer to BMS data structure to fill
 */
bool BMS_ParseFrame(uint32_t ext_id, uint32_t std_id,
                    const uint8_t *data, uint8_t dlc,
                    BMS_Data_t *bms);

/* Extended parser entry point used by diagnostics.  The legacy entry point
 * above remains the compatibility wrapper for existing host tests/callers. */
bool BMS_ParseFrameEx(uint32_t ext_id, uint32_t std_id,
                      const uint8_t *data, uint8_t dlc,
                      BMS_Data_t *bms,
                      BMS_ParseRejectReason_t *reason,
                      BMS_FrameType_t *frame_type);

/* ============== Control Info API ============== */

/**
 * @brief   Build Ctrl_INFO payload to send to BMS.
 * @param   out    8-byte output buffer
 * @param   ctrl   Control information
 */
void BMS_BuildCtrlInfo(uint8_t out[8], const BMS_CtrlInfo_t *ctrl);

#ifdef __cplusplus
}
#endif

#endif /* BMS_PROTOCOL_H */
