/**
 * @file    bsp_can.c
 * @brief   CAN BSP implementation using STM32G0 FDCAN API
 * @note    FDCAN1: 125Kbps - charger modules
 *          FDCAN2: 250Kbps - BMS
 */

#include "bsp_can.h"
#include "fdcan.h"
#include "debug_log.h"

volatile uint32_t g_c1_tx = 0, g_c1_rx = 0, g_c2_tx = 0, g_c2_rx = 0;
volatile uint32_t g_c1_tx_fail = 0, g_c2_tx_fail = 0;

static BSP_CAN_ChargerRxHandler_t g_charger_rx_handler = 0;
static BSP_CAN_BmsRxHandler_t     g_bms_rx_handler = 0;

void BSP_CAN_SetChargerRxHandler(BSP_CAN_ChargerRxHandler_t handler) {
    g_charger_rx_handler = handler;
}

void BSP_CAN_SetBmsRxHandler(BSP_CAN_BmsRxHandler_t handler) {
    g_bms_rx_handler = handler;
}

static void config_charger_bus_filters(FDCAN_HandleTypeDef *hfdcan)
{
    FDCAN_FilterTypeDef sFilterConfig;

    sFilterConfig.IdType = FDCAN_EXTENDED_ID;
    sFilterConfig.FilterIndex = 0;
    sFilterConfig.FilterType = FDCAN_FILTER_MASK;
    sFilterConfig.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    sFilterConfig.FilterID1 = 0x00000000;
    sFilterConfig.FilterID2 = 0x00000000;

    if (HAL_FDCAN_ConfigFilter(hfdcan, &sFilterConfig) != HAL_OK) return;
    if (HAL_FDCAN_ConfigGlobalFilter(hfdcan, FDCAN_REJECT, FDCAN_REJECT,
                                     FDCAN_FILTER_REMOTE, FDCAN_FILTER_REMOTE) != HAL_OK) return;
    if (HAL_FDCAN_Start(hfdcan) != HAL_OK) return;
    if (HAL_FDCAN_ActivateNotification(hfdcan, FDCAN_IT_RX_FIFO0_NEW_MESSAGE, 0) != HAL_OK) return;
}

static void config_bms_bus_filters(FDCAN_HandleTypeDef *hfdcan)
{
    FDCAN_FilterTypeDef sFilterConfig;

    /* Standard ID filter: accept-all → FIFO0 (mask 0 = don't care) */
    sFilterConfig.IdType = FDCAN_STANDARD_ID;
    sFilterConfig.FilterIndex = 0;
    sFilterConfig.FilterType = FDCAN_FILTER_MASK;
    sFilterConfig.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    sFilterConfig.FilterID1 = 0x000;
    sFilterConfig.FilterID2 = 0x000;
    if (HAL_FDCAN_ConfigFilter(hfdcan, &sFilterConfig) != HAL_OK) return;

    /* Extended ID filter: accept-all → FIFO0 */
    sFilterConfig.IdType = FDCAN_EXTENDED_ID;
    sFilterConfig.FilterIndex = 0;
    sFilterConfig.FilterType = FDCAN_FILTER_MASK;
    sFilterConfig.FilterConfig = FDCAN_FILTER_TO_RXFIFO0;
    sFilterConfig.FilterID1 = 0x00000000;
    sFilterConfig.FilterID2 = 0x00000000;
    if (HAL_FDCAN_ConfigFilter(hfdcan, &sFilterConfig) != HAL_OK) return;

    if (HAL_FDCAN_ConfigGlobalFilter(hfdcan, FDCAN_REJECT, FDCAN_REJECT,
                                     FDCAN_FILTER_REMOTE, FDCAN_FILTER_REMOTE) != HAL_OK) return;
    if (HAL_FDCAN_Start(hfdcan) != HAL_OK) return;
    if (HAL_FDCAN_ActivateNotification(hfdcan, FDCAN_IT_RX_FIFO0_NEW_MESSAGE, 0) != HAL_OK) return;
}

bool BSP_CAN_Start(void)
{
    config_charger_bus_filters(&hfdcan1);
    config_bms_bus_filters(&hfdcan2);
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
        /* BUGFIX (bonus finding while implementing B-10): BSP_CAN_Transmit()
         * is reached from every chg_lib driver's send_frame()-equivalent,
         * which runs inside CHG_LIB_Process()/CHG_LIB_FeedCanFrame()'s
         * critical section (see B-13). LOG() blocks on HAL_UART_Transmit()
         * for up to 50ms (Utils/Log/debug_log.c) -- calling it here would
         * block CAN RX for up to 50ms at exactly the moment the TX FIFO is
         * already full, i.e. under the heaviest bus load, worse still with
         * more modules (B-10). Count instead, same pattern as g_c1_tx/
         * g_c2_tx -- BSP_CAN_GetTxFailStats() exposes it for polling. */
        if (bus == 1) g_c1_tx_fail++;
        else if (bus == 2) g_c2_tx_fail++;
        return false;
    }

    if (bus == 1) g_c1_tx++;
    else if (bus == 2) g_c2_tx++;

    return true;
}

void HAL_FDCAN_RxFifo0Callback(FDCAN_HandleTypeDef *hfdcan, uint32_t RxFifo0ITs)
{
    if ((RxFifo0ITs & FDCAN_IT_RX_FIFO0_NEW_MESSAGE) != 0) {
        FDCAN_RxHeaderTypeDef RxHeader;
        uint8_t RxData[8];
        /* Drain all pending frames — interrupt flag is cleared by software,
         * so we must read until FIFO is empty to avoid lost messages. */
        while (HAL_FDCAN_GetRxMessage(hfdcan, FDCAN_RX_FIFO0, &RxHeader, RxData) == HAL_OK) {
            uint8_t actual_dlc = (uint8_t)(RxHeader.DataLength & 0x0FU);
            if (actual_dlc > 8U) actual_dlc = 8U;

            if (hfdcan->Instance == FDCAN1) {
                g_c1_rx++;
                if (RxHeader.IdType == FDCAN_EXTENDED_ID && g_charger_rx_handler != 0) {
                    g_charger_rx_handler(RxHeader.Identifier, RxData, actual_dlc);
                }
            } else if (hfdcan->Instance == FDCAN2) {
                g_c2_rx++;
                if (g_bms_rx_handler != 0) {
                    uint32_t ext_id = (RxHeader.IdType == FDCAN_EXTENDED_ID) ? RxHeader.Identifier : 0;
                    uint32_t std_id = (RxHeader.IdType == FDCAN_STANDARD_ID) ? RxHeader.Identifier : 0;
                    g_bms_rx_handler(ext_id, std_id, RxData, actual_dlc);
                }
            }
        }
    }
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
        HAL_FDCAN_ActivateNotification(&hfdcan1, FDCAN_IT_RX_FIFO0_NEW_MESSAGE, 0);
    }
    if (HAL_FDCAN_GetProtocolStatus(&hfdcan2, &ps) == HAL_OK && ps.BusOff) {
        LOG("[CAN2] BUS-OFF! Restart controller...\r\n");
        HAL_FDCAN_Stop(&hfdcan2);
        HAL_FDCAN_Start(&hfdcan2);
        HAL_FDCAN_ActivateNotification(&hfdcan2, FDCAN_IT_RX_FIFO0_NEW_MESSAGE, 0);
    }
}
