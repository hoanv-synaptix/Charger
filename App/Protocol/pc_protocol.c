/**
 * @file    pc_protocol.c
 * @brief   PC protocol handler - parse lệnh, gửi status, gọi Maxwell API v2
 */

#include "pc_protocol.h"
#include "chg_lib.h"
#include "bms_core.h"
#include "charge_cycle_config.h"
#include "charge_controller.h"
#include "usbd_cdc_if.h"
#include "debug_log.h"
#include "pc_debug_protocol.h"
#include <string.h>
#include <math.h>
#include "main.h"

/* ============== Private ============== */

static uint8_t g_charging = 0;
static float last_set_voltage = 0.0f;
static float last_set_current = 1.0f;

#define PC_TX_QUEUE_DEPTH 8U
#define PC_TX_FRAME_SIZE  (PC_MAX_PAYLOAD + 5U)

typedef struct {
    uint16_t len;
    uint8_t data[PC_TX_FRAME_SIZE];
} PcTxFrame_t;

static PcTxFrame_t g_tx_queue[PC_TX_QUEUE_DEPTH];
static volatile uint8_t g_tx_head = 0U;
static volatile uint8_t g_tx_tail = 0U;
static volatile uint8_t g_tx_count = 0U;
static volatile uint8_t g_tx_in_flight = 0U;
static volatile uint32_t g_tx_busy_count = 0U;
static volatile uint32_t g_tx_sent_count = 0U;

/* Forward declarations */
/* log_fixed helpers: kept but NOT called from ISR path (no LOG in ISR) */

static void apply_active_charge_profile(void)
{
    ChargeCycleConfig_t config;

    ChargeCycleConfig_Get(&config);
    if (config.charge_source_mode == CHARGE_SOURCE_STANDALONE_NO_BMS) {
        last_set_voltage = config.module_u_max_v;
        last_set_current = config.module_i_max_a;
        /* No LOG in ISR path — profile applied silently; main loop may log */
    }

    CHG_LIB_SetVoltageAll(last_set_voltage);
    CHG_LIB_SetCurrentLimitAll(last_set_current);
}

/* RX state machine */
typedef enum {
    ST_SOF1 = 0, ST_SOF2, ST_CMD, ST_LEN, ST_PAYLOAD, ST_CRC
} rx_state_t;

static rx_state_t rx_state = ST_SOF1;
static uint8_t rx_cmd, rx_len, rx_idx;
static uint8_t rx_payload[PC_MAX_PAYLOAD];

/* ============== CRC8 ============== */

static uint8_t crc8(const uint8_t *data, uint16_t len)
{
    uint8_t crc = 0;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ PC_CRC8_POLY) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

/* ============== TX helpers ============== */

static bool enqueue_frame(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    PcTxFrame_t *frame;
    uint16_t i = 0U;
    bool ret = false;
    __disable_irq();
    if (g_tx_count >= PC_TX_QUEUE_DEPTH) {
        /* Queue full: silently drop — no LOG in ISR */
        __enable_irq();
        return false;
    }

    frame = &g_tx_queue[g_tx_tail];
    frame->data[i++] = PC_SOF1;
    frame->data[i++] = PC_SOF2;
    frame->data[i++] = cmd;
    frame->data[i++] = len;
    for (uint8_t j = 0U; j < len; j++) {
        frame->data[i++] = payload[j];
    }
    frame->data[i++] = crc8(&frame->data[2], (uint16_t)(len + 2U));
    frame->len = i;
    g_tx_tail = (uint8_t)((g_tx_tail + 1U) % PC_TX_QUEUE_DEPTH);
    g_tx_count++;
    ret = true;
    __enable_irq();
    return ret;
}

static void send_frame(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    (void)enqueue_frame(cmd, payload, len);
}

void PC_Protocol_ProcessTx(void)
{
    uint8_t result;
    uint8_t head_snapshot;
    __disable_irq();
    if (g_tx_in_flight || g_tx_count == 0U) {
        __enable_irq();
        return;
    }
    head_snapshot = g_tx_head;
    __enable_irq();

    result = CDC_Transmit_FS(g_tx_queue[head_snapshot].data, g_tx_queue[head_snapshot].len);
    if (result == USBD_OK) {
        __disable_irq();
        g_tx_in_flight = 1U;
        g_tx_sent_count++;
        __enable_irq();
        /* Optional LOG removed for Tx path to reduce ISR noise; kept out of ISR anyway */
    } else if (result == USBD_BUSY) {
        __disable_irq();
        g_tx_busy_count++;
        __enable_irq();
    } else {
        /* Transmit error: no LOG spam */
    }
}

void PC_Protocol_NotifyTxComplete(void)
{
    __disable_irq();
    if (!g_tx_in_flight || g_tx_count == 0U) {
        __enable_irq();
        return;
    }

    g_tx_head = (uint8_t)((g_tx_head + 1U) % PC_TX_QUEUE_DEPTH);
    g_tx_count--;
    g_tx_in_flight = 0U;
    __enable_irq();
}

void PC_Protocol_ResetTx(void)
{
    __disable_irq();
    g_tx_head = 0U;
    g_tx_tail = 0U;
    g_tx_count = 0U;
    g_tx_in_flight = 0U;
    __enable_irq();
}

static void send_ack(uint8_t cmd)  { send_frame(PC_RSP_ACK, &cmd, 1); }
static void send_nack(uint8_t cmd, uint8_t err) {
    uint8_t p[2] = { cmd, err };
    send_frame(PC_RSP_NACK, p, 2);
}

static float payload_float(const uint8_t *p) {
    union { float f; uint8_t b[4]; } u;
    u.b[0]=p[0]; u.b[1]=p[1]; u.b[2]=p[2]; u.b[3]=p[3];
    return u.f;
}

static int32_t float_to_scaled_i32(float value, float scale)
{
    (void)value; (void)scale;
    return 0;
}

static void pack_u8(uint8_t value, uint8_t *out)
{
    out[0] = value;
}

static void pack_u32_le(uint32_t value, uint8_t *out)
{
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static void pack_float_le(float value, uint8_t *out)
{
    union { float f; uint8_t b[4]; } u;
    u.f = value;
    out[0] = u.b[0];
    out[1] = u.b[1];
    out[2] = u.b[2];
    out[3] = u.b[3];
}

static bool send_read_reg_response(uint8_t module_idx, uint16_t reg, uint8_t type,
                                   const uint8_t *data, uint8_t len)
{
    uint8_t payload[8];
    if (len > 4U) {
        return false;
    }

    payload[0] = module_idx;
    payload[1] = (uint8_t)(reg >> 8);
    payload[2] = (uint8_t)(reg & 0xFFU);
    payload[3] = type;
    if (len > 0U) {
        payload[4] = data[0];
        if (len > 1U) payload[5] = data[1];
        if (len > 2U) payload[6] = data[2];
        if (len > 3U) payload[7] = data[3];
    }
    send_frame(PC_RSP_READ_REG, payload, (uint8_t)(4U + len));
    return true;
}

static bool read_module_field(uint8_t module_idx, uint16_t reg)
{
    CHG_LIB_ModuleView_t view;
    uint8_t data[4];

    if (!CHG_LIB_GetModuleView(module_idx, &view) || !view.enabled) {
        return false;
    }

    switch (reg) {
    case 0x0001:
        pack_float_le(view.voltage, data);
        return send_read_reg_response(module_idx, reg, 0x01, data, 4);
    case 0x0002:
        pack_float_le(view.current, data);
        return send_read_reg_response(module_idx, reg, 0x01, data, 4);
    case 0x0003:
        pack_float_le(view.current_limit, data);
        return send_read_reg_response(module_idx, reg, 0x01, data, 4);
    case 0x0004:
        pack_float_le(view.temp_dcdc, data);
        return send_read_reg_response(module_idx, reg, 0x01, data, 4);
    case 0x000B:
        pack_float_le(view.temp_ambient, data);
        return send_read_reg_response(module_idx, reg, 0x01, data, 4);
    case 0x0040:
        pack_u32_le(view.alarm_status, data);
        return send_read_reg_response(module_idx, reg, 0x02, data, 4);
    case 0x0048:
        pack_u32_le(view.input_power, data);
        return send_read_reg_response(module_idx, reg, 0x02, data, 4);
    case 0x0100:
        pack_u8((uint8_t)view.state, data);
        return send_read_reg_response(module_idx, reg, 0x03, data, 1);
    case 0x0101:
        pack_u8(view.online ? 1U : 0U, data);
        return send_read_reg_response(module_idx, reg, 0x03, data, 1);
    case 0x0102:
        pack_u8(view.running ? 1U : 0U, data);
        return send_read_reg_response(module_idx, reg, 0x03, data, 1);
    case 0x0103:
        pack_u8(view.addr, data);
        return send_read_reg_response(module_idx, reg, 0x03, data, 1);
    case 0x0104:
        pack_u8(view.group, data);
        return send_read_reg_response(module_idx, reg, 0x03, data, 1);
    default:
        return false;
    }
}

/* ============== Command handler ============== */
/* NOTE: This function runs in USB ISR context (CDC_Receive_FS → FeedByte).
 * Do NOT call LOG (50ms blocking) here. Keep ISR minimal. */
static void process_frame(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    bool ok = false;

    switch (cmd) {
    case PC_CMD_SET_VOLTAGE:
        /* Block manual override when controller is auto-running */
        if (ChargeController_IsRunning() && !ChargeController_IsManualMode()) {
            send_nack(cmd, PC_ERR_BAD_PARAM);
            return;
        }
        if (len != 4) { send_nack(cmd, PC_ERR_BAD_LENGTH); return; }
        {
            float v = payload_float(payload);
            if (!isfinite(v)) { send_nack(cmd, PC_ERR_BAD_PARAM); return; }
            last_set_voltage = v;
        }
        ChargeController_SetManualTarget(last_set_voltage, last_set_current);
        CHG_LIB_SetVoltageAll(last_set_voltage);
        ok = true;
        break;

    case PC_CMD_SET_CURRENT:
        /* Block manual override when controller is auto-running */
        if (ChargeController_IsRunning() && !ChargeController_IsManualMode()) {
            send_nack(cmd, PC_ERR_BAD_PARAM);
            return;
        }
        if (len != 4) { send_nack(cmd, PC_ERR_BAD_LENGTH); return; }
        {
            float c = payload_float(payload);
            if (!isfinite(c)) { send_nack(cmd, PC_ERR_BAD_PARAM); return; }
            last_set_current = c;
        }
        ChargeController_SetManualTarget(last_set_voltage, last_set_current);
        CHG_LIB_SetCurrentLimitAll(last_set_current);
        ok = true;
        break;

    case PC_CMD_START: {
        uint8_t manual_mode = 0;
        if (len >= 1) {
            manual_mode = payload[0];
        }

        /* Check preconditions BEFORE accepting START */
        uint32_t faults = 0;
        if (!ChargeController_CheckPreconditions(&faults)) {
            send_nack(cmd, PC_ERR_BAD_PARAM);
            return;
        }

        /* Preconditions pass - request start */
        if (ChargeController_Start(CHARGE_CTRL_OWNER_PC, (bool)manual_mode)) {
            g_charging = 1;
            ok = true;
        } else {
            send_nack(cmd, PC_ERR_BAD_PARAM);
        }
        break;
    }

    case PC_CMD_STOP:
        ChargeController_Stop();
        g_charging = 0;
        ok = true;
        break;

    case PC_CMD_EMERGENCY_STOP:
        ChargeController_EmergencyStop();
        g_charging = 0;
        ok = true;
        break;

    case PC_CMD_SET_DRIVER:
        if (len != 1) { send_nack(cmd, PC_ERR_BAD_LENGTH); return; }
        if (!CHG_LIB_SelectDriver((CHG_LIB_DriverId_t)payload[0])) {
            send_nack(cmd, PC_ERR_CAN_TX_FAIL);
            return;
        }
        CHG_LIB_Init();
        ok = true;
        break;

    case PC_CMD_SET_MODULE_ADDR:
        if (len != 2) { send_nack(cmd, PC_ERR_BAD_LENGTH); return; }
        if (CHG_LIB_GetActiveDriverId() == CHG_LIB_DRV_NONE) {
            send_nack(cmd, PC_ERR_BAD_PARAM);
            return;
        }
        if (CHG_LIB_AddModule(payload[0], payload[1]) < 0) {
            send_nack(cmd, PC_ERR_BAD_PARAM);
            return;
        }
        /* Apply profile for manual mode; controller will override when started */
        apply_active_charge_profile();
        ok = true;
        break;

    case PC_CMD_READ_REG: {
        uint8_t module_idx;
        uint16_t reg;
        if (len != 3) { send_nack(cmd, PC_ERR_BAD_LENGTH); return; }
        module_idx = payload[0];
        reg = (uint16_t)(((uint16_t)payload[1] << 8) | payload[2]);
        if (!read_module_field(module_idx, reg)) {
            send_nack(cmd, PC_ERR_BAD_PARAM);
            return;
        }
        return;
    }

    case PC_CMD_PING:
        PC_Protocol_SendPong();
        return;

    default:
        /* Try debug protocol handler for debug commands */
        if (cmd >= DEBUG_CMD_ENTER && cmd <= DEBUG_CMD_SET_CHARGE_CFG) {
            if (DebugProtocol_HandleCommand(cmd, payload, len)) {
                return;
            }
        }
        send_nack(cmd, PC_ERR_UNKNOWN_CMD);
        return;
    }

    if (ok) send_ack(cmd);
    else    send_nack(cmd, PC_ERR_CAN_TX_FAIL);
}

/* ============== Public API ============== */

void PC_Protocol_FeedByte(uint8_t byte)
{
    /* Runs in USB ISR (CDC_Receive_FS) — no LOG allowed */
    switch (rx_state) {
    case ST_SOF1:
        if (byte == PC_SOF1) rx_state = ST_SOF2;
        break;
    case ST_SOF2:
        rx_state = (byte == PC_SOF2) ? ST_CMD : ST_SOF1;
        break;
    case ST_CMD:
        rx_cmd = byte; rx_state = ST_LEN;
        break;
    case ST_LEN:
        rx_len = byte; rx_idx = 0;
        if (rx_len > PC_MAX_PAYLOAD) rx_state = ST_SOF1;
        else if (rx_len == 0) rx_state = ST_CRC;
        else rx_state = ST_PAYLOAD;
        break;
    case ST_PAYLOAD:
        rx_payload[rx_idx++] = byte;
        if (rx_idx >= rx_len) rx_state = ST_CRC;
        break;
    case ST_CRC: {
        uint8_t buf[PC_MAX_PAYLOAD + 2];
        buf[0] = rx_cmd; buf[1] = rx_len;
        memcpy(&buf[2], rx_payload, rx_len);
        if (crc8(buf, rx_len + 2) == byte) {
            process_frame(rx_cmd, rx_payload, rx_len);
        } else {
            /* Bad CRC — no LOG in ISR */
            send_nack(rx_cmd, PC_ERR_BAD_CRC);
        }
        rx_state = ST_SOF1;
        break;
    }
    }
}

void PC_Protocol_SendStatus(void)
{
    CHG_LIB_SystemSummary_t sum;
    CHG_LIB_GetSystemSummary(&sum);

    /* Lấy temp cao nhất từ module đầu tiên online */
    float temp_dcdc = 0, temp_amb = 0;
    uint32_t alarm_or = 0;
    CHG_LIB_ModuleView_t view;
    for (uint8_t i = 0; i < CHG_LIB_GetModuleCount(); i++) {
        if (!CHG_LIB_GetModuleView(i, &view) || !view.enabled) continue;
        if (view.temp_dcdc > temp_dcdc) temp_dcdc = view.temp_dcdc;
        if (view.temp_ambient > temp_amb) temp_amb = view.temp_ambient;
        alarm_or |= view.alarm_status;
    }

    PC_StatusReport_t report;
    report.voltage        = sum.voltage;
    report.total_current  = sum.total_current;
    report.temp_dcdc      = temp_dcdc;
    report.temp_ambient   = temp_amb;
    report.alarm_status   = alarm_or;
    report.total_power_in = (uint32_t)sum.total_power_in;
    report.modules_online = sum.modules_online;
    report.modules_fault  = sum.modules_fault;
    report.charging       = ChargeController_IsRunning() ? 1 : 0;

    /* Populate BMS data */
    BMS_View_t bms_view;
    BMS_GetView(&bms_view);
    report.bms_voltage   = bms_view.batt_voltage;
    report.bms_current   = bms_view.batt_current;
    report.bms_chg_v_req = bms_view.chg_volt_request;
    report.bms_chg_i_req = bms_view.chg_curr_request;
    report.bms_alarm     = bms_view.alarm_flags;
    report.bms_soc       = bms_view.soc;
    report.bms_state     = (uint8_t)bms_view.state;
    report.btn_start      = 0; /* Sẽ cập nhật từ GPIO */
    report.btn_stop       = 0;

    send_frame(PC_RSP_STATUS, (uint8_t *)&report, sizeof(report));
}

void PC_Protocol_SendPong(void)
{
    uint32_t ver = ((uint32_t)FW_VERSION_MAJOR << 16) |
                   ((uint32_t)FW_VERSION_MINOR << 8) |
                   FW_VERSION_PATCH;
    send_frame(PC_RSP_PONG, (uint8_t *)&ver, 4);
}

void PC_Protocol_SendFrame(uint8_t cmd, const uint8_t *payload, uint16_t len)
{
    if (len > PC_MAX_PAYLOAD) len = PC_MAX_PAYLOAD;
    send_frame(cmd, payload, (uint8_t)len);
}

bool PC_Protocol_IsCharging(void)
{
    return ChargeController_IsRunning();
}








