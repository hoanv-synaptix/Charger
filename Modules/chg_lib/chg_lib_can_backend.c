/**
 * @file chg_lib_can_backend.c
 * @brief   CAN Backend implementation
 */

#include "chg_lib_can_backend.h"
#include "bsp_can.h"

extern uint32_t HAL_GetTick(void);

static const CHG_LIB_CanBackend_t *s_backend = 0;
static CHG_LIB_TxSource_t s_tx_source = CHG_LIB_TX_SOURCE_UNKNOWN;
static CHG_LIB_TxReason_t s_tx_reason = CHG_LIB_TX_REASON_UNKNOWN;
extern CHG_LIB_TxSource_t CHG_LIB_GetCommandSource(void);

static bool backend_transmit(uint32_t ext_id, const uint8_t *data, uint8_t dlc)
{
    BSP_CAN_Frame_t frame;
    frame.ext_id = ext_id;
    frame.dlc = dlc;
    frame.tx_source = (uint8_t)((s_tx_source != CHG_LIB_TX_SOURCE_UNKNOWN) ?
                                s_tx_source : CHG_LIB_GetCommandSource());
    frame.tx_reason = (uint8_t)s_tx_reason;
    for (uint8_t i = 0; i < dlc && i < 8; i++) {
        frame.data[i] = data[i];
    }
    return BSP_CAN_Transmit(1, &frame);
}

static uint32_t backend_now_tick(void)
{
    return HAL_GetTick();
}

static const CHG_LIB_CanBackend_t g_bsp_backend = {
    .transmit = backend_transmit,
    .now_tick = backend_now_tick,
};

const CHG_LIB_CanBackend_t *CHG_LIB_CanBackend_BSP(void)
{
    return &g_bsp_backend;
}

void CHG_LIB_CanBackend_Init(void)
{
    CHG_LIB_CanBackend_Set(&g_bsp_backend);
}

void CHG_LIB_CanBackend_Set(const CHG_LIB_CanBackend_t *backend)
{
    if (backend != 0 && backend->transmit != 0 && backend->now_tick != 0) {
        s_backend = backend;
    }
}

uint32_t CHG_LIB_CanBackend_NowTick(void)
{
    if (s_backend != 0 && s_backend->now_tick != 0) {
        return s_backend->now_tick();
    }
    extern uint32_t HAL_GetTick(void);
    return HAL_GetTick();
}

bool CHG_LIB_CanBackend_Transmit(uint32_t ext_id, const uint8_t *data, uint8_t dlc)
{
    if (s_backend != 0 && s_backend->transmit != 0) {
        return s_backend->transmit(ext_id, data, dlc);
    }
    return false;
}

bool CHG_LIB_CanBackend_TransmitEx(uint32_t ext_id, const uint8_t *data, uint8_t dlc,
                                   CHG_LIB_TxSource_t source)
{
    return CHG_LIB_CanBackend_TransmitMeta(ext_id, data, dlc, source,
                                           CHG_LIB_TX_REASON_UNKNOWN);
}

bool CHG_LIB_CanBackend_TransmitMeta(uint32_t ext_id, const uint8_t *data, uint8_t dlc,
                                     CHG_LIB_TxSource_t source,
                                     CHG_LIB_TxReason_t reason)
{
    CHG_LIB_TxSource_t previous_source = s_tx_source;
    CHG_LIB_TxReason_t previous_reason = s_tx_reason;
    bool result;

    s_tx_source = source;
    s_tx_reason = reason;
    result = CHG_LIB_CanBackend_Transmit(ext_id, data, dlc);
    s_tx_source = previous_source;
    s_tx_reason = previous_reason;
    return result;
}

void CHG_LIB_RecordRejectedZero(CHG_LIB_TxSource_t source,
                                CHG_LIB_CurrentPath_t path)
{
    BSP_CAN_RecordTxTraceReject((uint8_t)source, (uint8_t)path);
}

