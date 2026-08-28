/* Host-test stubs: LOG() passthrough to stdout, BSP_CAN_Transmit no-op,
 * USB CDC / flash-storage no-op stand-ins for App/Protocol host tests. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "bsp_can.h"
#include "usbd_cdc_if.h"
#include "charge_cycle_config.h"
#include "charge_cycle_storage.h"

void LOG(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

void LOG_Banner(void) {}
void LOG_TxCpltCallback(void) {}

bool BSP_CAN_Transmit(uint8_t bus, const BSP_CAN_Frame_t *frame)
{
    (void)bus;
    (void)frame;
    return true;
}

/* BSP/bsp_can.c's real CAN TX/RX counters -- pc_debug_protocol.c's
 * DebugProtocol_BuildSystemInfo() reads these via `extern`. bsp_can.c
 * itself isn't host-buildable (real FDCAN/HAL types), so stand in with
 * plain globals; tests that care can write to them directly. */
uint32_t g_c1_tx = 0, g_c1_rx = 0, g_c2_tx = 0, g_c2_rx = 0;

/* USB CDC: every transfer "succeeds" instantly (no real endpoint on host).
 * pc_protocol.c only checks the return value against USBD_OK/USBD_BUSY and
 * (elsewhere) reads hcdc->TxState -- neither is exercised meaningfully
 * without real USB hardware, so PC_Protocol_ProcessTx() itself is out of
 * scope for host tests; PC_Protocol_PeekTxFrame() is what host tests use
 * to inspect what got enqueued instead. Records the last transmit for the
 * rare test that does want to check it. */
USBD_HandleTypeDef hUsbDeviceFS;
uint8_t g_last_cdc_tx[512];
uint16_t g_last_cdc_tx_len = 0;

uint8_t CDC_Transmit_FS(uint8_t *Buf, uint16_t Len)
{
    uint16_t copy_len = (Len < sizeof(g_last_cdc_tx)) ? Len : (uint16_t)sizeof(g_last_cdc_tx);
    memcpy(g_last_cdc_tx, Buf, copy_len);
    g_last_cdc_tx_len = Len;
    return (uint8_t)USBD_OK;
}

/* Flash persistence: host tests never verify real flash write/erase
 * (needs real hardware -- see AUDIT_Findings.md I-07/I-10 for the class of
 * item that's intentionally left for hardware verification). Standing in
 * with RAM-only "always succeeds" stubs is enough to exercise the protocol
 * dispatch logic that calls these (DEBUG_CMD_SET_CHARGE_CFG, PC_CMD_SET_DRIVER). */
bool g_storage_save_called = false;

void ChargeCycleStorage_Init(void) {}

bool ChargeCycleStorage_Load(ChargeCycleConfig_t *config)
{
    (void)config;
    return false; /* "flash invalid" -- caller keeps RAM defaults, same as real cold-boot */
}

bool ChargeCycleStorage_Save(const ChargeCycleConfig_t *config)
{
    (void)config;
    g_storage_save_called = true;
    return true;
}
