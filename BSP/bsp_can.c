/**
 * @file    bsp_can.c
 * @brief   CAN BSP implementation using STM32G0 FDCAN API
 * @note    FDCAN1: 125Kbps - charger modules
 *          FDCAN2: 250Kbps - BMS
 */

#include "bsp_can.h"
#include "bsp_sys.h"
#include "fdcan.h"
#include "debug_log.h"
#include <string.h>

volatile uint32_t g_c1_tx = 0, g_c1_rx = 0, g_c2_tx = 0, g_c2_rx = 0;
volatile uint32_t g_c1_tx_fail = 0, g_c2_tx_fail = 0;

static BSP_CAN_ChargerRxHandler_t g_charger_rx_handler = 0;
static BSP_CAN_BmsRxHandler_t     g_bms_rx_handler = 0;

/*
 * RX transport contract:
 *   - the FDCAN callback is the producer and only copies raw frames;
 *   - the main loop is the consumer and owns protocol processing;
 *   - one spare slot makes the usable SPSC capacity exactly 32 frames.
 *
 * The queue is intentionally private to the BSP.  Protocol modules must not
 * depend on FDCAN headers or interrupt context.
 */
#define BSP_CAN_RX_QUEUE_CAPACITY  32U
#define BSP_CAN_RX_QUEUE_STORAGE   (BSP_CAN_RX_QUEUE_CAPACITY + 1U)
#define BSP_CAN_RX_PROCESS_BUDGET  16U
#define BSP_CAN_RX_ISR_DRAIN_LIMIT 8U
#define BSP_CAN_RX_MAX_QUEUE_AGE_MS 1000U

#if defined(CHG_DEBUG_CAN_TX_TRACE)
#define BSP_CAN_TX_TRACE_CAPACITY 32U

typedef struct {
    uint32_t id;
    uint8_t dlc;
    uint8_t data[8];
} BSP_CAN_TxTraceFrame_t;

static BSP_CAN_TxTraceFrame_t g_tx_trace[BSP_CAN_TX_TRACE_CAPACITY];
static uint8_t g_tx_trace_head;
static uint8_t g_tx_trace_tail;

static uint8_t tx_trace_next(uint8_t index)
{
    index++;
    return (index >= BSP_CAN_TX_TRACE_CAPACITY) ? 0U : index;
}

static bool is_module_control_frame(uint32_t id)
{
    uint8_t pf = (uint8_t)((id >> 16) & 0xFFU);
    return pf == 0x04U || pf == 0x06U; /* C_M_2 / C_M_24 */
}

static void tx_trace_reset(void)
{
    g_tx_trace_head = 0U;
    g_tx_trace_tail = 0U;
}

static void tx_trace_push(const BSP_CAN_Frame_t *frame)
{
    uint8_t next;

    if (frame == NULL || !is_module_control_frame(frame->ext_id)) return;

    next = tx_trace_next(g_tx_trace_head);
    if (next == g_tx_trace_tail) {
        /* Drop newest diagnostic record only; CAN transmission already
         * succeeded and must never depend on the trace buffer. */
        return;
    }

    g_tx_trace[g_tx_trace_head].id = frame->ext_id;
    g_tx_trace[g_tx_trace_head].dlc = (frame->dlc > 8U) ? 8U : frame->dlc;
    memcpy(g_tx_trace[g_tx_trace_head].data, frame->data, 8U);
    g_tx_trace_head = next;
}

static bool tx_trace_pop(BSP_CAN_TxTraceFrame_t *frame)
{
    if (frame == NULL || g_tx_trace_tail == g_tx_trace_head) return false;
    *frame = g_tx_trace[g_tx_trace_tail];
    g_tx_trace_tail = tx_trace_next(g_tx_trace_tail);
    return true;
}
#endif

#define BSP_CAN_RX_NOTIFICATIONS (FDCAN_IT_RX_FIFO0_NEW_MESSAGE | \
                                  FDCAN_IT_RX_FIFO0_FULL | \
                                  FDCAN_IT_RX_FIFO0_MESSAGE_LOST | \
                                  FDCAN_IT_ERROR_PASSIVE | \
                                  FDCAN_IT_ERROR_WARNING | \
                                  FDCAN_IT_BUS_OFF | \
                                  FDCAN_IT_ARB_PROTOCOL_ERROR | \
                                  FDCAN_IT_DATA_PROTOCOL_ERROR)

typedef struct {
    BSP_CAN_RxFrame_t frames[BSP_CAN_RX_QUEUE_STORAGE];
    volatile uint8_t head;
    volatile uint8_t tail;
    volatile BSP_CAN_RxStats_t stats;
} BSP_CAN_RxQueue_t;

static BSP_CAN_RxQueue_t g_c1_rx_queue;
static BSP_CAN_RxQueue_t g_c2_rx_queue;

static uint8_t queue_next(uint8_t index)
{
    index++;
    return (index >= BSP_CAN_RX_QUEUE_STORAGE) ? 0U : index;
}

static void queue_reset(BSP_CAN_RxQueue_t *queue)
{
    queue->head = 0U;
    queue->tail = 0U;
    memset((void *)&queue->stats, 0, sizeof(queue->stats));
}

static void queue_push_from_isr(BSP_CAN_RxQueue_t *queue,
                                const BSP_CAN_RxFrame_t *frame)
{
    uint8_t head = queue->head;
    uint8_t next = queue_next(head);

    if (next == queue->tail) {
        /* Drop newest to preserve frame ordering for protocol state machines. */
        queue->stats.queue_overflow_count++;
        return;
    }

    queue->frames[head] = *frame;
    queue->head = next;
    queue->stats.queued_rx_count++;
}

static bool queue_pop(BSP_CAN_RxQueue_t *queue, BSP_CAN_RxFrame_t *frame)
{
    uint8_t tail = queue->tail;

    if (tail == queue->head) {
        return false;
    }

    *frame = queue->frames[tail];
    queue->tail = queue_next(tail);
    return true;
}

static BSP_CAN_RxQueue_t *queue_for_handle(const FDCAN_HandleTypeDef *hfdcan)
{
    return (hfdcan->Instance == FDCAN1) ? &g_c1_rx_queue : &g_c2_rx_queue;
}

static uint8_t bus_for_handle(const FDCAN_HandleTypeDef *hfdcan)
{
    return (hfdcan->Instance == FDCAN1) ? 1U : 2U;
}

static bool activate_notifications(FDCAN_HandleTypeDef *hfdcan)
{
    return HAL_FDCAN_ActivateNotification(hfdcan, BSP_CAN_RX_NOTIFICATIONS, 0U) == HAL_OK;
}

void BSP_CAN_SetChargerRxHandler(BSP_CAN_ChargerRxHandler_t handler) {
    g_charger_rx_handler = handler;
}

void BSP_CAN_SetBmsRxHandler(BSP_CAN_BmsRxHandler_t handler) {
    g_bms_rx_handler = handler;
}

static bool config_charger_bus_filters(FDCAN_HandleTypeDef *hfdcan)
{
    FDCAN_FilterTypeDef sFilterConfig;

    sFilterConfig.IdType = FDCAN_EXTENDED_ID;
    sFilterConfig.FilterIndex = 0;
    sFilterConfig.FilterType = FDCAN_FILTER_MASK;
    sFilterConfig.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    sFilterConfig.FilterID1 = 0x00000000;
    sFilterConfig.FilterID2 = 0x00000000;

    if (HAL_FDCAN_ConfigFilter(hfdcan, &sFilterConfig) != HAL_OK) return false;
    if (HAL_FDCAN_ConfigGlobalFilter(hfdcan, FDCAN_REJECT, FDCAN_REJECT,
                                     FDCAN_FILTER_REMOTE, FDCAN_FILTER_REMOTE) != HAL_OK) return false;
    if (HAL_FDCAN_Start(hfdcan) != HAL_OK) return false;
    return activate_notifications(hfdcan);
}

static bool config_bms_bus_filters(FDCAN_HandleTypeDef *hfdcan)
{
    FDCAN_FilterTypeDef sFilterConfig;

    /* Standard ID filter: accept-all → FIFO0 (mask 0 = don't care) */
    sFilterConfig.IdType = FDCAN_STANDARD_ID;
    sFilterConfig.FilterIndex = 0;
    sFilterConfig.FilterType = FDCAN_FILTER_MASK;
    sFilterConfig.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    sFilterConfig.FilterID1 = 0x000;
    sFilterConfig.FilterID2 = 0x000;
    if (HAL_FDCAN_ConfigFilter(hfdcan, &sFilterConfig) != HAL_OK) return false;

    /* Extended ID filter: accept-all → FIFO0 */
    sFilterConfig.IdType = FDCAN_EXTENDED_ID;
    sFilterConfig.FilterIndex = 0;
    sFilterConfig.FilterType = FDCAN_FILTER_MASK;
    sFilterConfig.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    sFilterConfig.FilterID1 = 0x00000000;
    sFilterConfig.FilterID2 = 0x00000000;
    if (HAL_FDCAN_ConfigFilter(hfdcan, &sFilterConfig) != HAL_OK) return false;

    if (HAL_FDCAN_ConfigGlobalFilter(hfdcan, FDCAN_REJECT, FDCAN_REJECT,
                                     FDCAN_FILTER_REMOTE, FDCAN_FILTER_REMOTE) != HAL_OK) return false;
    if (HAL_FDCAN_Start(hfdcan) != HAL_OK) return false;
    return activate_notifications(hfdcan);
}

bool BSP_CAN_Start(void)
{
    queue_reset(&g_c1_rx_queue);
    queue_reset(&g_c2_rx_queue);
#if defined(CHG_DEBUG_CAN_TX_TRACE)
    tx_trace_reset();
#endif

    if (!config_charger_bus_filters(&hfdcan1)) return false;
    if (!config_bms_bus_filters(&hfdcan2)) return false;
    return true;
}

bool BSP_CAN_Transmit(uint8_t bus, const BSP_CAN_Frame_t *frame)
{
    FDCAN_TxHeaderTypeDef TxHeader;
    TxHeader.Identifier = frame->ext_id;
    TxHeader.IdType = FDCAN_EXTENDED_ID;
    TxHeader.TxFrameType = FDCAN_DATA_FRAME;

    switch(frame->dlc) {
        case 0: TxHeader.DataLength = FDCAN_DLC_BYTES_0; break;
        case 1: TxHeader.DataLength = FDCAN_DLC_BYTES_1; break;
        case 2: TxHeader.DataLength = FDCAN_DLC_BYTES_2; break;
        case 3: TxHeader.DataLength = FDCAN_DLC_BYTES_3; break;
        case 4: TxHeader.DataLength = FDCAN_DLC_BYTES_4; break;
        case 5: TxHeader.DataLength = FDCAN_DLC_BYTES_5; break;
        case 6: TxHeader.DataLength = FDCAN_DLC_BYTES_6; break;
        case 7: TxHeader.DataLength = FDCAN_DLC_BYTES_7; break;
        case 8: TxHeader.DataLength = FDCAN_DLC_BYTES_8; break;
        default: TxHeader.DataLength = FDCAN_DLC_BYTES_8; break;
    }

    TxHeader.ErrorStateIndicator = FDCAN_ESI_ACTIVE;
    TxHeader.BitRateSwitch = FDCAN_BRS_OFF;
    TxHeader.FDFormat = FDCAN_CLASSIC_CAN;
    TxHeader.TxEventFifoControl = FDCAN_NO_TX_EVENTS;
    TxHeader.MessageMarker = 0;

    if (HAL_FDCAN_AddMessageToTxFifoQ((bus == 1) ? &hfdcan1 : &hfdcan2,
                                      &TxHeader, (uint8_t *)frame->data) != HAL_OK) {
        /* Do not log here: a formatted UART log can block the timing-sensitive
         * CAN path.  Count the failure for diagnostic polling instead. */
        if (bus == 1) g_c1_tx_fail++;
        else if (bus == 2) g_c2_tx_fail++;
        return false;
    }

    if (bus == 1) g_c1_tx++;
    else if (bus == 2) g_c2_tx++;

#if defined(CHG_DEBUG_CAN_TX_TRACE)
    if (bus == 1U) tx_trace_push(frame);
#endif

    return true;
}

void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *hfdcan, uint32_t RxFifo0ITs)
{
    BSP_CAN_RxQueue_t *queue = queue_for_handle(hfdcan);
    uint8_t bus = bus_for_handle(hfdcan);

    if ((RxFifo0ITs & FDCAN_IT_RX_FIFO0_MESSAGE_LOST) != 0U) {
        queue->stats.fifo_lost_count++;
    }
    if ((RxFifo0ITs & FDCAN_IT_RX_FIFO0_FULL) != 0U) {
        queue->stats.fifo_full_count++;
    }

    if ((RxFifo0ITs & (FDCAN_IT_RX_FIFO0_NEW_MESSAGE |
                       FDCAN_IT_RX_FIFO0_FULL |
                       FDCAN_IT_RX_FIFO0_MESSAGE_LOST)) != 0U) {
        FDCAN_RxHeaderTypeDef RxHeader;
        uint8_t RxData[8];
        uint8_t drain_count = 0U;

        /* Capture only.  The finite bound prevents a saturated bus from
         * keeping the CPU in the interrupt indefinitely. */
        while (drain_count < BSP_CAN_RX_ISR_DRAIN_LIMIT &&
               HAL_FDCAN_GetRxMessage(hfdcan, FDCAN_RX_FIFO0, &RxHeader, RxData) == HAL_OK) {
            uint8_t actual_dlc = (uint8_t)(RxHeader.DataLength & 0x0FU);
            if (actual_dlc > 8U) actual_dlc = 8U;
            BSP_CAN_RxFrame_t frame;

            frame.id = RxHeader.Identifier;
            frame.is_extended = (RxHeader.IdType == FDCAN_EXTENDED_ID);
            frame.dlc = actual_dlc;
            frame.rx_tick = BSP_GetTick();
            memcpy(frame.data, RxData, sizeof(frame.data));

            queue->stats.raw_rx_count++;
            queue_push_from_isr(queue, &frame);
            drain_count++;
        }

        if (bus == 1U) g_c1_rx += drain_count;
        else if (bus == 2U) g_c2_rx += drain_count;
    }
}

void HAL_FDCAN_ErrorStatusCallback(FDCAN_HandleTypeDef *hfdcan, uint32_t ErrorStatusITs)
{
    BSP_CAN_RxQueue_t *queue = queue_for_handle(hfdcan);

    if ((ErrorStatusITs & FDCAN_IT_BUS_OFF) != 0U) {
        queue->stats.bus_off_count++;
    }
    if ((ErrorStatusITs & FDCAN_IT_ERROR_WARNING) != 0U) {
        queue->stats.error_warning_count++;
    }
    if ((ErrorStatusITs & FDCAN_IT_ERROR_PASSIVE) != 0U) {
        queue->stats.error_passive_count++;
    }
    if ((ErrorStatusITs & (FDCAN_IT_ARB_PROTOCOL_ERROR |
                           FDCAN_IT_DATA_PROTOCOL_ERROR)) != 0U) {
        queue->stats.protocol_error_count++;
    }
}

static void process_rx_queue(BSP_CAN_RxQueue_t *queue, uint8_t bus)
{
    BSP_CAN_RxFrame_t frame;
    uint8_t processed = 0U;

    while (processed < BSP_CAN_RX_PROCESS_BUDGET && queue_pop(queue, &frame)) {
        if ((uint32_t)(BSP_GetTick() - frame.rx_tick) > BSP_CAN_RX_MAX_QUEUE_AGE_MS) {
            queue->stats.stale_queue_drop_count++;
            processed++;
            continue;
        }

        if (bus == 1U) {
            if (frame.is_extended && g_charger_rx_handler != 0) {
                g_charger_rx_handler(frame.id, frame.data, frame.dlc);
            }
        } else if (bus == 2U && g_bms_rx_handler != 0) {
            uint32_t ext_id = frame.is_extended ? frame.id : 0U;
            uint32_t std_id = frame.is_extended ? 0U : frame.id;
            g_bms_rx_handler(ext_id, std_id, frame.data, frame.dlc);
        }
        processed++;
    }
}

void BSP_CAN_ProcessRx(void)
{
    process_rx_queue(&g_c1_rx_queue, 1U);
    process_rx_queue(&g_c2_rx_queue, 2U);
}

void BSP_CAN_ProcessTxTrace(void)
{
#if defined(CHG_DEBUG_CAN_TX_TRACE)
    BSP_CAN_TxTraceFrame_t frame;
    uint8_t budget = 2U;

    while (budget-- > 0U && tx_trace_pop(&frame)) {
        uint8_t pf = (uint8_t)((frame.id >> 16) & 0xFFU);

        if (pf == 0x04U) {
            uint16_t voltage_raw = (uint16_t)frame.data[4] |
                                    ((uint16_t)frame.data[5] << 8);
            uint16_t current_raw = (uint16_t)frame.data[6] |
                                    ((uint16_t)frame.data[7] << 8);
            const char *setpoint_reason =
                (voltage_raw == 0U && current_raw == 0U) ? "START_RESET" :
                (current_raw == 0U) ? "CURRENT_ZERO" : "NORMAL";
            LOG("[CAN1 TX] C_M_2 reason=%s id=%08lX dlc=%u Vraw=%u Iraw=%u "
                "data=%02X %02X %02X %02X %02X %02X %02X %02X\r\n",
                setpoint_reason,
                (unsigned long)frame.id, (unsigned)frame.dlc,
                (unsigned)voltage_raw, (unsigned)current_raw,
                frame.data[0], frame.data[1], frame.data[2], frame.data[3],
                frame.data[4], frame.data[5], frame.data[6], frame.data[7]);
        } else {
            const char *command = (frame.data[0] == 0x55U) ? "STOP" :
                                  (frame.data[0] == 0xAAU) ? "START" : "OTHER";
            LOG("[CAN1 TX] C_M_24 id=%08lX dlc=%u cmd=%s(0x%02X) "
                "data=%02X %02X %02X %02X %02X %02X %02X %02X\r\n",
                (unsigned long)frame.id, (unsigned)frame.dlc, command,
                (unsigned)frame.data[0],
                frame.data[0], frame.data[1], frame.data[2], frame.data[3],
                frame.data[4], frame.data[5], frame.data[6], frame.data[7]);
        }
    }
#else
    /* Keep the call site stable between normal and diagnostic builds. */
#endif
}

void BSP_CAN_GetStats(uint32_t *c1tx, uint32_t *c1rx, uint32_t *c2tx, uint32_t *c2rx)
{
    if (c1tx) *c1tx = g_c1_tx;
    if (c1rx) *c1rx = g_c1_rx;
    if (c2tx) *c2tx = g_c2_tx;
    if (c2rx) *c2rx = g_c2_rx;
}

void BSP_CAN_GetTxFailStats(uint32_t *c1_fail, uint32_t *c2_fail)
{
    if (c1_fail) *c1_fail = g_c1_tx_fail;
    if (c2_fail) *c2_fail = g_c2_tx_fail;
}

void BSP_CAN_GetRxStats(uint8_t bus, BSP_CAN_RxStats_t *stats)
{
    BSP_CAN_RxQueue_t *queue;

    if (stats == 0 || (bus != 1U && bus != 2U)) return;
    queue = (bus == 1U) ? &g_c1_rx_queue : &g_c2_rx_queue;

    BSP_EnterCritical();
    *stats = *(const BSP_CAN_RxStats_t *)&queue->stats;
    BSP_ExitCritical();
}

/* Watchdog bus-off: neu controller roi vao bus-off (khong ai ACK trong lau),
 * restart de khoi phuc truyen nhan. */
void BSP_CAN_Process(void)
{
    static uint32_t last_check = 0;
    uint32_t now = HAL_GetTick();
    if ((now - last_check) < 1000) return;
    last_check = now;

    FDCAN_ProtocolStatusTypeDef ps;

    if (HAL_FDCAN_GetProtocolStatus(&hfdcan1, &ps) == HAL_OK && ps.BusOff) {
        LOG("[CAN1] BUS-OFF! Restart controller...\r\n");
        HAL_FDCAN_Stop(&hfdcan1);
        HAL_FDCAN_Start(&hfdcan1);
        activate_notifications(&hfdcan1);
    }
    if (HAL_FDCAN_GetProtocolStatus(&hfdcan2, &ps) == HAL_OK && ps.BusOff) {
        LOG("[CAN2] BUS-OFF! Restart controller...\r\n");
        HAL_FDCAN_Stop(&hfdcan2);
        HAL_FDCAN_Start(&hfdcan2);
        activate_notifications(&hfdcan2);
    }
}
