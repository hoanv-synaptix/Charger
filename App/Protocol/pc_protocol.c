/**
 * @file    pc_protocol.c
 * @brief   PC protocol handler - parse lệnh, gửi status, gọi Maxwell API v2
 */

#include "pc_protocol.h"
#include "chg_lib.h"
#include "bms_core.h"
#include "charge_cycle_config.h"
#include "charge_cycle_storage.h"
#include "charge_controller.h"
#include "ota_service.h"
#include "sd_storage.h"
#include "usbd_cdc_if.h"
#include "debug_log.h"
#include "quectel_at_engine.h"
#include "pc_debug_protocol.h"
#include <string.h>
#include <math.h>
#include "bsp_sys.h"

/* ============== Private ============== */

static uint8_t g_charging = 0;
static float last_set_voltage = 0.0f;
static float last_set_current = 1.0f;

#define PC_TX_QUEUE_DEPTH 16U
#define PC_TX_FRAME_SIZE  (PC_MAX_PAYLOAD + 5U)
#define PC_RX_QUEUE_DEPTH 4U

typedef struct {
    uint16_t len;
    uint8_t data[PC_TX_FRAME_SIZE];
} PcTxFrame_t;

typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint8_t crc_ok;
    uint8_t payload[PC_MAX_PAYLOAD];
} PcRxFrame_t;

static PcTxFrame_t g_tx_queue[PC_TX_QUEUE_DEPTH];
static volatile uint8_t g_tx_head = 0U;
static volatile uint8_t g_tx_tail = 0U;
static volatile uint8_t g_tx_count = 0U;
static volatile uint8_t g_tx_in_flight = 0U;
static volatile uint32_t g_tx_busy_count = 0U;
static volatile uint32_t g_tx_sent_count = 0U;

static PcRxFrame_t g_rx_queue[PC_RX_QUEUE_DEPTH];
static volatile uint8_t g_rx_head = 0U;
static volatile uint8_t g_rx_tail = 0U;
static volatile uint8_t g_rx_count = 0U;
static volatile uint32_t g_rx_overflow_count = 0U;

/* Forward declarations */
/* log_fixed helpers: kept but NOT called from ISR path (no LOG in ISR) */

static bool apply_active_charge_profile(void)
{
    ChargeCycleConfig_t config;
    float profile_voltage = last_set_voltage;
    float profile_current = last_set_current;

    ChargeCycleConfig_Get(&config);
    if (config.charge_source_mode == CHARGE_SOURCE_STANDALONE_NO_BMS) {
        profile_voltage = config.module_u_max_v;
        profile_current = config.module_i_max_a;
        /* No LOG in ISR path — profile applied silently; main loop may log */
    }

    if (!CHG_LIB_SetCurrentLimitAllEx(profile_current, CHG_LIB_TX_SOURCE_PC_PROFILE)) {
        return false;
    }
    last_set_voltage = profile_voltage;
    last_set_current = profile_current;
    CHG_LIB_SetVoltageAllEx(profile_voltage, CHG_LIB_TX_SOURCE_PC_PROFILE);
    return true;
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
    BSP_EnterCritical();
    if (g_tx_count >= PC_TX_QUEUE_DEPTH) {
        /* Queue full: drop oldest frame, but only if not currently in-flight
         * USB transfer (dropping in-flight would corrupt the USB pointer). */
        if (g_tx_in_flight) {
            BSP_ExitCritical();
            return false; /* Cannot enqueue — TX busy and queue full */
        }
        g_tx_head = (uint8_t)((g_tx_head + 1U) % PC_TX_QUEUE_DEPTH);
        g_tx_count--;
    }

    /* Write to the current tail index, then increment tail */
    frame = &g_tx_queue[g_tx_tail];

    frame->data[i++] = PC_SOF1;
    frame->data[i++] = PC_SOF2;
    frame->data[i++] = cmd;
    frame->data[i++] = len;

    if (len > 0U && payload != NULL) {
        memcpy(&frame->data[i], payload, len);
        i += len;
    }

    frame->data[i++] = crc8(&frame->data[2], (uint16_t)(len + 2U));
    frame->len = i;
    
    g_tx_tail = (uint8_t)((g_tx_tail + 1U) % PC_TX_QUEUE_DEPTH);
    g_tx_count++;
    ret = true;
    BSP_ExitCritical();
    /* No LOG here — may be called from USB ISR context */
    return ret;
}

static void send_frame(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    (void)enqueue_frame(cmd, payload, len);
}

static bool enqueue_rx_frame(uint8_t cmd, const uint8_t *payload, uint8_t len, bool crc_ok)
{
    bool ret = false;

    BSP_EnterCritical();
    if (g_rx_count < PC_RX_QUEUE_DEPTH) {
        PcRxFrame_t *frame = &g_rx_queue[g_rx_tail];
        frame->cmd = cmd;
        frame->len = len;
        frame->crc_ok = crc_ok ? 1U : 0U;
        if (len > 0U && payload != NULL) {
            memcpy(frame->payload, payload, len);
        }
        g_rx_tail = (uint8_t)((g_rx_tail + 1U) % PC_RX_QUEUE_DEPTH);
        g_rx_count++;
        ret = true;
    } else {
        /* Drop newest. The ISR must remain bounded and must not log. */
        g_rx_overflow_count++;
    }
    BSP_ExitCritical();
    return ret;
}

static bool dequeue_rx_frame(PcRxFrame_t *frame)
{
    bool ret = false;

    if (frame == NULL) {
        return false;
    }

    BSP_EnterCritical();
    if (g_rx_count > 0U) {
        *frame = g_rx_queue[g_rx_head];
        g_rx_head = (uint8_t)((g_rx_head + 1U) % PC_RX_QUEUE_DEPTH);
        g_rx_count--;
        ret = true;
    }
    BSP_ExitCritical();
    return ret;
}

static USBD_CDC_HandleTypeDef *get_cdc_handle(void)
{
    extern USBD_HandleTypeDef hUsbDeviceFS;
    if (hUsbDeviceFS.pClassDataCmsit[0] != NULL) {
        return (USBD_CDC_HandleTypeDef*)hUsbDeviceFS.pClassDataCmsit[0];
    }
    if (hUsbDeviceFS.pClassData != NULL) {
        return (USBD_CDC_HandleTypeDef*)hUsbDeviceFS.pClassData;
    }
    return NULL;
}

void PC_Protocol_ProcessTx(void)
{
    uint8_t result;
    uint8_t head_snapshot;
    static uint32_t last_tx_start = 0;
    extern USBD_HandleTypeDef hUsbDeviceFS;
    USBD_CDC_HandleTypeDef *hcdc = get_cdc_handle();

    uint32_t now_tick = BSP_GetTick();

    if (g_tx_count > 0) {
        if (hcdc == NULL) {
            static uint32_t last_err_tick = 0;
            if (now_tick - last_err_tick > 1000) {
                last_err_tick = now_tick;
                /* Only log error state, not every poll — avoids 50ms UART block */
            }
            return;
        }
        /* Removed verbose TX TRY log — adds 50ms latency to 20ms main loop */
    }

    if (hcdc == NULL) {
        return;
    }

    BSP_EnterCritical();
    if (g_tx_in_flight || hcdc->TxState != 0U) {
        /* Timeout: proportional to frame size. USB FS sends 64B/pkt,
         * ~1ms/pkt. 50ms base + 1ms per 8 bytes covers large frames
         * even with host buffering delays. */
        uint32_t frame_len = g_tx_queue[g_tx_head].len;
        uint32_t timeout_ms = 50U + (frame_len / 8U);
        if (BSP_GetTick() - last_tx_start > timeout_ms) {
            /* Timeout: Host did not pull data. Reset TX state to unblock */
            hcdc->TxState = 0U;
            g_tx_in_flight = 0U;
            if (g_tx_count > 0) {
                g_tx_head = (uint8_t)((g_tx_head + 1U) % PC_TX_QUEUE_DEPTH);
                g_tx_count--;
            }
            BSP_ExitCritical();
            /* Removed verbose TX TIMEOUT log — adds 50ms latency */
            return;
        } else {
            BSP_ExitCritical();
            return;
        }
    }
    if (g_tx_count == 0U) {
        BSP_ExitCritical();
        return;
    }
    head_snapshot = g_tx_head;
    g_tx_in_flight = 1U;
    last_tx_start = BSP_GetTick();
    BSP_ExitCritical();

    result = CDC_Transmit_FS(g_tx_queue[head_snapshot].data, g_tx_queue[head_snapshot].len);
    if (result == USBD_OK) {
        BSP_EnterCritical();
        g_tx_sent_count++;
        BSP_ExitCritical();
        /* Removed verbose TX OK log — adds 50ms latency to 20ms main loop */
    } else {
        BSP_EnterCritical();
        g_tx_in_flight = 0U;
        if (result == USBD_BUSY) {
            g_tx_busy_count++;
        }
        BSP_ExitCritical();
        LOG("[PC TX FAIL] res=%u cmd=0x%02X\r\n", result, g_tx_queue[head_snapshot].data[2]);
    }
}

void PC_Protocol_NotifyTxComplete(void)
{
    BSP_EnterCritical();
    if (!g_tx_in_flight || g_tx_count == 0U) {
        BSP_ExitCritical();
        return;
    }

    g_tx_head = (uint8_t)((g_tx_head + 1U) % PC_TX_QUEUE_DEPTH);
    g_tx_count--;
    g_tx_in_flight = 0U;
    BSP_ExitCritical();
}

void PC_Protocol_ResetTx(void)
{
    USBD_CDC_HandleTypeDef *hcdc = get_cdc_handle();
    BSP_EnterCritical();
    if (hcdc != NULL) {
        hcdc->TxState = 0U;
    }
    g_tx_head = 0U;
    g_tx_tail = 0U;
    g_tx_count = 0U;
    g_tx_in_flight = 0U;
    BSP_ExitCritical();
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

static uint32_t unpack_u32_le(const uint8_t *in)
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
           ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
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
/* Runs in main-loop context after PC_Protocol_ProcessRx() dequeues a complete
 * frame. Hardware and application command handling is deliberately kept out
 * of the USB receive ISR. */
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
            if (!CHG_LIB_SetCurrentLimitAllEx(c, CHG_LIB_TX_SOURCE_PC_SET_CURRENT)) {
                send_nack(cmd, PC_ERR_BAD_PARAM);
                return;
            }
            last_set_current = c;
        }
        ChargeController_SetManualTarget(last_set_voltage, last_set_current);
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
        if (ChargeController_Start(CHARGE_CTRL_OWNER_PC, (bool)manual_mode, BSP_GetTick())) {
            g_charging = 1;
            ok = true;
        } else {
            send_nack(cmd, PC_ERR_BAD_PARAM);
        }
        break;
    }

    case PC_CMD_STOP: {
        ChargeCtrlView_t view;
        ChargeController_GetView(&view);
        if (view.state == CHARGE_CTRL_STATE_FAULT) {
            (void)ChargeController_ResetFaultIfSafe(BSP_GetTick());
        } else {
            ChargeController_Stop(BSP_GetTick());
        }
        g_charging = 0;
        ok = true;
        break;
    }

    case PC_CMD_EMERGENCY_STOP:
        ChargeController_EmergencyStop(BSP_GetTick());
        g_charging = 0;
        ok = true;
        break;

    case 0x0A: /* PC_CMD_RESET_FAULT */
        (void)ChargeController_ResetFaultIfSafe(BSP_GetTick());
        ok = true;
        break;

    case PC_CMD_SET_OTA_POLICY: {
        char manifest_url[128];
        uint32_t interval_ms = 0U;
        uint8_t enabled;
        if (len < 1U) { send_nack(cmd, PC_ERR_BAD_LENGTH); return; }
        enabled = payload[0];
        if (enabled > 1U) { send_nack(cmd, PC_ERR_BAD_PARAM); return; }
        if (enabled != 0U) {
            size_t url_len;
            if (len < 5U) { send_nack(cmd, PC_ERR_BAD_LENGTH); return; }
            interval_ms = unpack_u32_le(&payload[1]);
            url_len = (size_t)len - 5U;
            if (url_len == 0U || url_len >= sizeof(manifest_url)) {
                send_nack(cmd, PC_ERR_BAD_LENGTH);
                return;
            }
            memcpy(manifest_url, &payload[5], url_len);
            manifest_url[url_len] = '\0';
        }
        if (!OTAService_SetPolicy(enabled != 0U, interval_ms,
                                  enabled != 0U ? manifest_url : NULL)) {
            send_nack(cmd, PC_ERR_BAD_PARAM);
            return;
        }
        ok = true;
        break;
    }

    case PC_CMD_GET_OTA_STATUS: {
        OtaStatusView_t status;
        if (len != 0U) { send_nack(cmd, PC_ERR_BAD_LENGTH); return; }
        OTAService_GetStatus(&status);
        send_frame(PC_RSP_OTA_STATUS, (const uint8_t *)&status, sizeof(status));
        return;
    }

    case PC_CMD_OTA_CHECK_NOW: {
        if (len != 0U) { send_nack(cmd, PC_ERR_BAD_LENGTH); return; }
        OtaCheckResult_t res = OTAService_RequestCheckNowResult();
        if (res == OTA_CHECK_OK) {
            send_ack(cmd);
        } else {
            uint8_t err_code = PC_ERR_CAN_TX_FAIL;
            switch (res) {
            case OTA_CHECK_ERR_POLICY_DISABLED: err_code = PC_ERR_OTA_POLICY_DISABLED; break;
            case OTA_CHECK_ERR_NOT_SAFE:         err_code = PC_ERR_OTA_NOT_SAFE; break;
            case OTA_CHECK_ERR_FLASH_BUSY:      err_code = PC_ERR_OTA_FLASH_BUSY; break;
            case OTA_CHECK_ERR_NET_NOT_READY:   err_code = PC_ERR_OTA_NET_NOT_READY; break;
            case OTA_CHECK_ERR_BUSY:            err_code = PC_ERR_OTA_BUSY; break;
            default:                            err_code = PC_ERR_BAD_PARAM; break;
            }
            send_nack(cmd, err_code);
        }
        return;
    }

    case PC_CMD_OTA_APPLY:
        if (len != 0U) { send_nack(cmd, PC_ERR_BAD_LENGTH); return; }
        ok = OTAService_RequestApply();
        if (ok) {
            send_ack(cmd);
            PC_Protocol_ProcessTx();
            HAL_Delay(100);
            NVIC_SystemReset();
            return;
        }
        break;

    case PC_CMD_TEST_FLASH: {
        if (len != 0U) { send_nack(cmd, PC_ERR_BAD_LENGTH); return; }
        uint32_t jedec = 0U;
        uint32_t cap_kb = 0U;
        bool test_ok = OTAService_SelfTestFlash(&jedec, &cap_kb);
        uint8_t rsp[9];
        rsp[0] = test_ok ? 1U : 0U;
        rsp[1] = (uint8_t)(jedec & 0xFFU);
        rsp[2] = (uint8_t)((jedec >> 8) & 0xFFU);
        rsp[3] = (uint8_t)((jedec >> 16) & 0xFFU);
        rsp[4] = (uint8_t)((jedec >> 24) & 0xFFU);
        rsp[5] = (uint8_t)(cap_kb & 0xFFU);
        rsp[6] = (uint8_t)((cap_kb >> 8) & 0xFFU);
        rsp[7] = (uint8_t)((cap_kb >> 16) & 0xFFU);
        rsp[8] = (uint8_t)((cap_kb >> 24) & 0xFFU);
        send_frame(PC_RSP_FLASH_TEST, rsp, sizeof(rsp));
        return;
    }

    case PC_CMD_TEST_SD: {
        SDStorageTestResult_t test;
        uint8_t rsp[16];

        if (len != 0U) { send_nack(cmd, PC_ERR_BAD_LENGTH); return; }
        if (ChargeController_IsRunning()) {
            send_nack(cmd, PC_ERR_BAD_PARAM);
            return;
        }

        (void)SDStorage_RunSelfTest(&test);
        rsp[0] = test.card_present ? 1U : 0U;
        rsp[1] = test.card_ready ? 1U : 0U;
        rsp[2] = test.mounted ? 1U : 0U;
        rsp[3] = test.sector0_read ? 1U : 0U;
        rsp[4] = test.mbr_signature ? 1U : 0U;
        rsp[5] = test.file_write ? 1U : 0U;
        rsp[6] = test.file_read ? 1U : 0U;
        rsp[7] = test.file_match ? 1U : 0U;
        rsp[8] = test.file_removed ? 1U : 0U;
        rsp[9] = (uint8_t)test.last_result;
        rsp[10] = (uint8_t)(test.block_count & 0xFFU);
        rsp[11] = (uint8_t)((test.block_count >> 8U) & 0xFFU);
        rsp[12] = (uint8_t)((test.block_count >> 16U) & 0xFFU);
        rsp[13] = (uint8_t)((test.block_count >> 24U) & 0xFFU);
        rsp[14] = (uint8_t)test.read_stage;
        rsp[15] = test.read_response;
        send_frame(PC_RSP_SD_TEST, rsp, sizeof(rsp));
        return;
    }

    case PC_CMD_OTA_UPLOAD_START: {
        if (len != 12U) { send_nack(cmd, PC_ERR_BAD_LENGTH); return; }
        uint32_t total_size = unpack_u32_le(&payload[0]);
        uint32_t expected_crc = unpack_u32_le(&payload[4]);
        uint32_t version = unpack_u32_le(&payload[8]);
        if (!OTAService_DirectUploadStart(total_size, expected_crc, version)) {
            send_nack(cmd, PC_ERR_BAD_PARAM);
            return;
        }
        ok = true;
        break;
    }

    case PC_CMD_OTA_UPLOAD_CHUNK: {
        if (len < 5U) { send_nack(cmd, PC_ERR_BAD_LENGTH); return; }
        uint32_t offset = unpack_u32_le(&payload[0]);
        uint16_t chunk_len = (uint16_t)(len - 4U);
        if (!OTAService_DirectUploadChunk(offset, &payload[4], chunk_len)) {
            send_nack(cmd, PC_ERR_BAD_PARAM);
            return;
        }
        ok = true;
        break;
    }

    case PC_CMD_OTA_UPLOAD_FINISH: {
        if (len != 0U) { send_nack(cmd, PC_ERR_BAD_LENGTH); return; }
        if (!OTAService_DirectUploadFinish()) {
            send_nack(cmd, PC_ERR_BAD_PARAM);
            return;
        }
        ok = true;
        break;
    }

    case PC_CMD_GET_4G_STATUS: {
        if (len != 0U) { send_nack(cmd, PC_ERR_BAD_LENGTH); return; }
        QuectelNetStatus_t status;
        QuectelEngine_GetStatus(&status);
        uint8_t rsp[50];
        memset(rsp, 0, sizeof(rsp));
        rsp[0] = (uint8_t)status.state;
        rsp[1] = status.powered ? 1U : 0U;
        rsp[2] = status.sim_ready ? 1U : 0U;
        rsp[3] = status.net_registered ? 1U : 0U;
        rsp[4] = status.pdp_active ? 1U : 0U;
        rsp[5] = status.csq_rssi;
        memcpy(&rsp[6], status.ip_addr, 20U);
        memcpy(&rsp[26], status.model, 24U);
        send_frame(PC_RSP_4G_STATUS, rsp, sizeof(rsp));
        return;
    }

    case PC_CMD_SET_DRIVER:
        if (len != 1) { send_nack(cmd, PC_ERR_BAD_LENGTH); return; }
        if (!CHG_LIB_SelectDriver((CHG_LIB_DriverId_t)payload[0])) {
            send_nack(cmd, PC_ERR_BAD_PARAM);
            return;
        }
        /* Persist driver choice to flash so it survives reboot.
         * driver_id 1=MAXWELL,2=LIANMING,3=TONHE → module_type +1 */
        {
            ChargeCycleConfig_t cfg;
            ChargeCycleConfig_Get(&cfg);
            cfg.module_type = (uint8_t)payload[0] + 1U;
            ChargeCycleConfig_Set(&cfg);
            ChargeCycleStorage_Save(&cfg);
        }
        /* BUGFIX: ChargeCycleConfig_Set() above is called here only to
         * persist module_type -- but it also auto-registers
         * cfg.source_module_count modules at default addr=1..N (see its
         * own comment in charge_cycle_config.c), which is correct when the
         * config is the sole source of module topology (boot-time load,
         * DEBUG_CMD_SET_CHARGE_CFG) but wrong here: the PC app's real flow
         * is SET_DRIVER followed by its OWN explicit PC_CMD_SET_MODULE_ADDR
         * call(s) for the actual module address(es) (debug_app/main.py's
         * _sync_driver()/_sync_module_addr()). Without clearing that side
         * effect, a leftover source_module_count from a prior session
         * silently registers a phantom module at addr=1; if the user's
         * real module address happens to collide, the following real
         * SET_MODULE_ADDR is silently rejected (CHG_LIB_AddModule()
         * returns -1 on an addr collision) instead of registering the
         * real module -- otherwise it just leaves a stray phantom module
         * alongside the real one. CHG_LIB_Init() here (moved from before
         * the config-persist block to after it) clears that side effect so
         * SET_MODULE_ADDR is the sole source of truth for module topology
         * after a driver change, matching the app's actual usage. Found
         * and confirmed via test/host_protocol_sim/test_pc_protocol_e2e.c's
         * test_set_driver_and_module_addr_over_wire. */
        CHG_LIB_Init();
        ok = true;
        break;

    case PC_CMD_SET_MODULE_ADDR: {
        if (len != 2) { send_nack(cmd, PC_ERR_BAD_LENGTH); return; }
        if (CHG_LIB_GetActiveDriverId() == CHG_LIB_DRV_NONE) {
            send_nack(cmd, PC_ERR_BAD_PARAM);
            return;
        }
        int8_t idx = CHG_LIB_AddModule(payload[0], payload[1]);
        if (idx < 0) {
            send_nack(cmd, PC_ERR_BAD_PARAM);
            return;
        }
        /* Seed rated current from config -- see the matching comment in
         * charge_cycle_config.c's ChargeCycleConfig_Set() for why. */
        {
            ChargeCycleConfig_t mcfg;
            ChargeCycleConfig_Get(&mcfg);
            if (mcfg.module_i_max_a > 0.0f) {
                CHG_LIB_SetModuleConfig((uint8_t)idx, mcfg.module_i_max_a);
            }
        }
        /* Apply profile for manual mode; controller will override when started */
        if (!apply_active_charge_profile()) {
            send_nack(cmd, PC_ERR_BAD_PARAM);
            return;
        }
        ok = true;
        break;
    }

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
        if (cmd >= DEBUG_CMD_ENTER && cmd <= DEBUG_CMD_RESET_TOTALS) {
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
    /* Runs from CDC_Receive_FS. Only bounded parsing and queueing are allowed
     * here; command handlers run from PC_Protocol_ProcessRx() in main. */
    switch (rx_state) {
    case ST_SOF1:
        if (byte == PC_SOF1) rx_state = ST_SOF2;
        break;
    case ST_SOF2:
        /* B-02 fix: a repeated PC_SOF1 byte (e.g. AA AA 55) must be treated
         * as the start of a new frame, not just a resync miss -- otherwise
         * a stray/duplicated SOF1 byte on the wire forces the receiver to
         * wait for a third SOF1 before it can lock on again. */
        if (byte == PC_SOF2) rx_state = ST_CMD;
        else rx_state = (byte == PC_SOF1) ? ST_SOF2 : ST_SOF1;
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
            (void)enqueue_rx_frame(rx_cmd, rx_payload, rx_len, true);
        } else {
            (void)enqueue_rx_frame(rx_cmd, NULL, 0U, false);
        }
        rx_state = ST_SOF1;
        break;
    }
    }
}

void PC_Protocol_ProcessRx(void)
{
    PcRxFrame_t frame;
    uint8_t processed = 0U;

    while (processed < PC_RX_QUEUE_DEPTH && dequeue_rx_frame(&frame)) {
        processed++;
        if (frame.crc_ok == 0U) {
            send_nack(frame.cmd, PC_ERR_BAD_CRC);
        } else {
            process_frame(frame.cmd, frame.payload, frame.len);
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
    /* Preserve the public telemetry union. Safety decisions are made from
     * BMS alarm_flags by the controller/alarm path; warning_flags remains a
     * compatibility field in the existing PC payload. */
    report.bms_alarm     = bms_view.alarm_flags | bms_view.warning_flags;
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

uint8_t PC_Protocol_GetTxQueueDepth(void)
{
    return g_tx_count;
}

bool PC_Protocol_PeekTxFrame(uint8_t index, uint8_t *cmd, uint8_t *payload, uint8_t *payload_len)
{
    if (index >= g_tx_count) {
        return false;
    }
    uint8_t slot = (uint8_t)((g_tx_head + index) % PC_TX_QUEUE_DEPTH);
    PcTxFrame_t *frame = &g_tx_queue[slot];
    uint8_t len = frame->data[3]; /* [SOF1][SOF2][CMD][LEN][PAYLOAD...][CRC] */
    if (cmd != NULL) *cmd = frame->data[2];
    if (payload_len != NULL) *payload_len = len;
    if (payload != NULL && len > 0U) {
        memcpy(payload, &frame->data[4], len);
    }
    return true;
}
