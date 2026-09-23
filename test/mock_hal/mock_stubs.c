/* Host-test stubs: LOG() passthrough to stdout, BSP_CAN_Transmit no-op,
 * USB CDC / flash-storage no-op stand-ins for App/Protocol host tests. */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "bsp_can.h"
#include "usbd_cdc_if.h"
#include "charge_cycle_config.h"
#include "charge_cycle_storage.h"
#include "charge_energy_storage.h"
#include "sd_storage.h"

typedef struct {
    uint32_t status;
    uint32_t version;
    uint32_t image_size;
    uint32_t downloaded_bytes;
    uint32_t boot_request;
    uint32_t boot_attempts;
    uint32_t policy_enabled;
} OtaStatusView_t;

typedef struct {
    uint8_t state;
    bool powered;
    bool sim_ready;
    bool net_registered;
    bool pdp_active;
    uint8_t csq_rssi;
    char ip_addr[20];
    char model[24];
} QuectelNetStatus_t;

void LOG(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

void LOG_Banner(void) {}
void LOG_TxCpltCallback(void) {}

/* The PC protocol host test exercises command dispatch but not the target
 * flash journal implementation. Keep its storage boundary deterministic. */
bool ChargeEnergyStorage_Reset(void) { return true; }

/* Spy on the last-transmitted frame per extended CAN ID, across both
 * buses -- lets a test assert on frame *content* (e.g. Ctrl_INFO's chg_sw
 * byte) without a full CAN-bus model. Small, fixed-size table; only the
 * IDs a test actually looks up need a slot. Zeroed at process start same
 * as any other global -- tests that care should reset the fields they
 * check before driving, same discipline as the sim_bms/sim_can_modules
 * globals. */
#define MOCK_CAN_SPY_SLOTS 4
typedef struct { uint32_t ext_id; uint8_t data[8]; uint8_t dlc; bool seen; } MockCanSpySlot_t;
static MockCanSpySlot_t g_can_spy[MOCK_CAN_SPY_SLOTS];

bool BSP_CAN_Transmit(uint8_t bus, const BSP_CAN_Frame_t *frame)
{
    (void)bus;
    for (int i = 0; i < MOCK_CAN_SPY_SLOTS; i++) {
        if (g_can_spy[i].seen && g_can_spy[i].ext_id == frame->ext_id) {
            memcpy(g_can_spy[i].data, frame->data, 8);
            g_can_spy[i].dlc = frame->dlc;
            return true;
        }
    }
    for (int i = 0; i < MOCK_CAN_SPY_SLOTS; i++) {
        if (!g_can_spy[i].seen) {
            g_can_spy[i].ext_id = frame->ext_id;
            memcpy(g_can_spy[i].data, frame->data, 8);
            g_can_spy[i].dlc = frame->dlc;
            g_can_spy[i].seen = true;
            return true;
        }
    }
    return true; /* spy table full -- silently drop tracking, TX still "succeeds" */
}

void BSP_CAN_RecordTxTraceReject(uint8_t source, uint8_t path)
{
    (void)source;
    (void)path;
}

/* Returns true and fills *data_out (8 bytes) if a frame with this ext_id
 * has been transmitted at least once via BSP_CAN_Transmit(); false if
 * never seen. */
bool MockCan_GetLastTx(uint32_t ext_id, uint8_t data_out[8])
{
    for (int i = 0; i < MOCK_CAN_SPY_SLOTS; i++) {
        if (g_can_spy[i].seen && g_can_spy[i].ext_id == ext_id) {
            memcpy(data_out, g_can_spy[i].data, 8);
            return true;
        }
    }
    return false;
}

/* BSP/bsp_can.c's real CAN TX/RX counters -- pc_debug_protocol.c's
 * DebugProtocol_BuildSystemInfo() reads these via BSP_CAN_GetStats().
 * bsp_can.c itself isn't host-buildable (real FDCAN/HAL types), so stand
 * in with plain globals + the same accessor signature; tests that care
 * can write to the globals directly. */
uint32_t g_c1_tx = 0, g_c1_rx = 0, g_c2_tx = 0, g_c2_rx = 0;

void BSP_CAN_GetStats(uint32_t *c1tx, uint32_t *c1rx, uint32_t *c2tx, uint32_t *c2rx)
{
    if (c1tx) *c1tx = g_c1_tx;
    if (c1rx) *c1rx = g_c1_rx;
    if (c2tx) *c2tx = g_c2_tx;
    if (c2rx) *c2rx = g_c2_rx;
}

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

bool ChargeCycleStorage_SaveProfile(uint8_t mode, const ChargeCycleConfig_t *config)
{
    (void)mode;
    (void)config;
    g_storage_save_called = true;
    return true;
}

/* Mock RTC stubs for host test */
#include "bsp_rtc.h"

static uint32_t s_mock_rtc_epoch = 1772866800U;
static bool s_mock_rtc_valid = false;

bool BSP_RTC_Init(void) { return true; }
bool BSP_RTC_IsTimeValid(void) { return s_mock_rtc_valid; }
bool BSP_RTC_SetEpoch(uint32_t epoch) { s_mock_rtc_epoch = epoch; s_mock_rtc_valid = true; return true; }
uint32_t BSP_RTC_GetEpoch(void) { return s_mock_rtc_epoch; }
bool BSP_RTC_GetDateTime(BSP_RTC_DateTime_t *dt) {
    if (!dt) return false;
    dt->year = 2026;
    dt->month = 3;
    dt->day = 7;
    dt->hour = 7;
    dt->minute = 0;
    dt->second = 0;
    dt->weekday = 6;
    return true;
}
bool BSP_RTC_SetDateTime(const BSP_RTC_DateTime_t *dt) {
    (void)dt;
    s_mock_rtc_valid = true;
    return true;
}
void BSP_RTC_FormatTime(char *buf, size_t buf_size) {
    if (buf && buf_size > 0) snprintf(buf, buf_size, "07:00:00");
}
void BSP_RTC_FormatDateTime(char *buf, size_t buf_size) {
    if (buf && buf_size > 0) snprintf(buf, buf_size, "2026-03-07 07:00:00");
}

/* Convert Unix epoch to broken-down calendar (Gregorian, no leap seconds).
 * Minimal implementation sufficient for host-test purposes. */
uint32_t BSP_RTC_DateTimeToEpoch(const BSP_RTC_DateTime_t *dt)
{
    if (!dt) return 0U;
    /* Days from epoch (1970-01-01) to start of the given year */
    uint32_t y = dt->year;
    uint32_t m = dt->month;
    uint32_t d = dt->day;
    /* Move Jan/Feb to previous year for leap-day calculation */
    if (m <= 2U) { y--; m += 12U; }
    uint32_t leap = (y / 4U) - (y / 100U) + (y / 400U);
    uint32_t days = 365U * dt->year + leap
                  + (306U * (m + 1U)) / 10U - 428U
                  + d - 719163U; /* offset to 1970-01-01 */
    return days * 86400U
         + (uint32_t)dt->hour   * 3600U
         + (uint32_t)dt->minute * 60U
         + (uint32_t)dt->second;
}

void BSP_RTC_EpochToDateTime(uint32_t epoch, BSP_RTC_DateTime_t *dt)
{
    if (!dt) return;
    /* Algorithm: Euclidean affine transforms (Richards, 2013) */
    uint32_t z  = epoch / 86400U + 719468U;
    uint32_t era = z / 146097U;
    uint32_t doe = z - era * 146097U;
    uint32_t yoe = (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
    uint32_t y   = yoe + era * 400U;
    uint32_t doy = doe - (365U * yoe + yoe / 4U - yoe / 100U);
    uint32_t mp  = (5U * doy + 2U) / 153U;
    uint32_t d   = doy - (153U * mp + 2U) / 5U + 1U;
    uint32_t m   = (mp < 10U) ? (mp + 3U) : (mp - 9U);
    if (m <= 2U) y++;
    uint32_t rem = epoch % 86400U;
    dt->year    = (uint16_t)y;
    dt->month   = (uint8_t)m;
    dt->day     = (uint8_t)d;
    dt->hour    = (uint8_t)(rem / 3600U);
    dt->minute  = (uint8_t)((rem % 3600U) / 60U);
    dt->second  = (uint8_t)(rem % 60U);
    dt->weekday = 0U; /* not used in host tests */
}

/* Mock AlarmStorage stubs for host test */
uint8_t AlarmStorage_Init(void *ram_log, uint8_t max_entries, uint32_t *out_sequence)
{
    (void)ram_log;
    (void)max_entries;
    if (out_sequence) *out_sequence = 0;
    return 0;
}

bool AlarmStorage_Append(uint32_t now_tick, uint16_t code, uint8_t action, bool raised)
{
    (void)now_tick;
    (void)code;
    (void)action;
    (void)raised;
    return true;
}

/* Mock BSP_SPIFlash stubs for host test */
bool BSP_SPIFlash_IsAvailable(void) { return false; }
bool BSP_SPIFlash_Read(uint32_t a, uint8_t *b, uint32_t l) { (void)a; (void)b; (void)l; return false; }
bool BSP_SPIFlash_Write(uint32_t a, const uint8_t *d, uint32_t l) { (void)a; (void)d; (void)l; return false; }
bool BSP_SPIFlash_EraseSector4K(uint32_t a) { (void)a; return false; }

/* OTA command boundary stubs for the PC protocol host suite. The real OTA
 * service is exercised by the target firmware build; this test only verifies
 * PC framing and command dispatch. */
bool OTAService_SetPolicy(bool enabled, uint32_t interval_ms, const char *url)
{
    (void)enabled; (void)interval_ms; (void)url; return true;
}
bool OTAService_RequestCheckNow(void) { return true; }
bool OTAService_RequestCheckNowResult(uint8_t *result_code) { if (result_code) *result_code = 0; return true; }
bool OTAService_RequestApply(void) { return true; }
void OTAService_GetStatus(OtaStatusView_t *status)
{
    if (status != NULL) memset(status, 0, sizeof(*status));
}
bool OTAService_SelfTestFlash(uint32_t *out_jedec, uint32_t *out_cap_kb)
{
    if (out_jedec != NULL) *out_jedec = 0x856017U;
    if (out_cap_kb != NULL) *out_cap_kb = 8192U;
    return true;
}
bool OTAService_DirectUploadStart(uint32_t total_size, uint32_t expected_crc32, uint32_t version)
{
    (void)total_size; (void)expected_crc32; (void)version; return true;
}
bool OTAService_DirectUploadChunk(uint32_t offset, const uint8_t *data, uint16_t len)
{
    (void)offset; (void)data; (void)len; return true;
}
bool OTAService_DirectUploadFinish(void) { return true; }

void QuectelEngine_GetStatus(QuectelNetStatus_t *out_status)
{
    if (out_status != NULL) memset(out_status, 0, sizeof(*out_status));
}

bool SDStorage_RunSelfTest(SDStorageTestResult_t *result)
{
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
        result->last_result = FR_NOT_READY;
    }
    return false;
}

void HAL_Delay(uint32_t ms) { (void)ms; }
void NVIC_SystemReset(void) {}
