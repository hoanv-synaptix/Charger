/**
 * @file ota_service.c
 * @brief Bounded OTA download state machine.
 *
 * Quectel QHTTPREAD is deliberately handled as two protocols: AT text until
 * CONNECT, then an exact-length binary body, then AT text again. The binary
 * body is never passed through ReadLine().
 */

#include "ota_service.h"
#include "main.h"
#include "bms_core.h"
#include "charge_controller.h"
#include "chg_lib.h"
#include "bsp_spi_flash.h"
#include "bsp_quectel.h"
#include "quectel_at_engine.h"
#include "ota_sha256.h"
#include "debug_log.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define OTA_URL_MAX_LENGTH       ((uint32_t)127U)
#define OTA_STREAM_CHUNK_SIZE    256U
#define OTA_ERASE_TIMEOUT_MS     10000U
#define OTA_AT_TIMEOUT_MS        12000U  /* AT cmds on EC200U can take up to 10s */
#define OTA_HTTP_TIMEOUT_MS      90000U  /* HTTP GET/READ requests over 4G */
#define OTA_BODY_TIMEOUT_MS      45000U  /* Stream chunk timeout tolerating 4G cellular jitter */
#define OTA_CURRENT_SAFE_A       0.5f
#define OTA_DEFAULT_CHECK_INTERVAL_MS (6UL * 60UL * 60UL * 1000UL)

static OtaDescriptor_t s_desc;
static char s_url[OTA_URL_MAX_LENGTH + 1U];
static uint32_t s_step_tick;
static uint32_t s_erase_index;
static uint32_t s_erase_count;
static uint32_t s_crc_state;
static OtaSha256Context_t s_sha256;
static bool s_manifest_mode;
static uint32_t s_read_expected_bytes;
static uint32_t s_manifest_received;
static uint8_t s_manifest_buf[768];
static bool s_policy_enabled;
static uint32_t s_policy_interval_ms;
static uint32_t s_next_policy_tick;
static char s_policy_manifest_url[OTA_URL_MAX_LENGTH + 1U];

typedef enum {
    OTA_STEP_IDLE = 0,
    OTA_STEP_ERASE_START,
    OTA_STEP_ERASE_WAIT,
    OTA_STEP_CHECK_PDP,          /* AT+QIACT? — verify PDP context active before QHTTP */
    OTA_STEP_WAIT_CHECK_PDP,
    OTA_STEP_ACTIVATE_PDP,       /* AT+QIACT=1 — re-activate if not active */
    OTA_STEP_WAIT_ACTIVATE_PDP,
    OTA_STEP_CFG_HTTP_CTX,
    OTA_STEP_WAIT_CFG_HTTP_CTX,
    OTA_STEP_CFG_HTTP_SSL,
    OTA_STEP_WAIT_CFG_HTTP_SSL,
    OTA_STEP_CFG_SSL_SNI,
    OTA_STEP_WAIT_CFG_SSL_SNI,
    OTA_STEP_CFG_SSL_SEC,
    OTA_STEP_WAIT_CFG_SSL_SEC,
    OTA_STEP_SET_URL_CMD,
    OTA_STEP_WAIT_CONNECT_URL,
    OTA_STEP_SEND_URL_BODY,
    OTA_STEP_WAIT_GET_RESP,
    OTA_STEP_WAIT_READ_CONNECT,
    OTA_STEP_READING_STREAM,
    OTA_STEP_WAIT_READ_DONE,
    OTA_STEP_VERIFY,
    OTA_STEP_ERROR
} OtaStep_t;

static OtaStep_t s_step = OTA_STEP_IDLE;

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    for (size_t i = 0U; i < len; ++i) {
        crc ^= data[i];
        for (uint32_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return crc;
}

static uint32_t descriptor_crc(const OtaDescriptor_t *desc)
{
    return crc32_update(0xFFFFFFFFU, (const uint8_t *)desc,
                        sizeof(*desc) - sizeof(desc->header_crc32)) ^ 0xFFFFFFFFU;
}

static void reset_descriptor(void)
{
    memset(&s_desc, 0, sizeof(s_desc));
    s_desc.magic = OTA_MAGIC_HEADER;
    s_desc.descriptor_version = OTA_DESCRIPTOR_VERSION;
    s_desc.status = OTA_STATUS_IDLE;
    s_desc.target_mcu = OTA_TARGET_STM32G0B1;
    s_desc.health_marker = OTA_HEALTH_MARKER_PENDING;
}

static bool valid_descriptor(const OtaDescriptor_t *desc)
{
    return desc->magic == OTA_MAGIC_HEADER &&
           desc->descriptor_version == OTA_DESCRIPTOR_VERSION &&
           desc->target_mcu == OTA_TARGET_STM32G0B1 &&
           desc->header_crc32 == descriptor_crc(desc);
}

static bool read_descriptor_at(uint32_t address, OtaDescriptor_t *out)
{
    if (!BSP_SPIFlash_Read(address, (uint8_t *)out, sizeof(*out))) return false;
    return valid_descriptor(out);
}

static bool save_descriptor(void)
{
    OtaDescriptor_t next = s_desc;
    OtaDescriptor_t primary;
    OtaDescriptor_t mirror;
    uint32_t target = SPI_FLASH_OTA_META_BASE;
    bool primary_ok = read_descriptor_at(SPI_FLASH_OTA_META_BASE, &primary);
    bool mirror_ok = read_descriptor_at(SPI_FLASH_OTA_META_MIRROR_BASE, &mirror);

    if (primary_ok && (!mirror_ok || primary.transaction_id >= mirror.transaction_id)) {
        target = SPI_FLASH_OTA_META_MIRROR_BASE;
    } else if (mirror_ok) {
        target = SPI_FLASH_OTA_META_BASE;
    }

    next.transaction_id = (primary_ok && mirror_ok)
        ? ((primary.transaction_id > mirror.transaction_id) ? primary.transaction_id : mirror.transaction_id) + 1U
        : (primary_ok ? primary.transaction_id + 1U : (mirror_ok ? mirror.transaction_id + 1U : 1U));
    next.header_crc32 = descriptor_crc(&next);
    if (!BSP_SPIFlash_EraseSector4K(target)) return false;
    if (!BSP_SPIFlash_Write(target, (const uint8_t *)&next, sizeof(next))) return false;
    s_desc = next;
    return true;
}

static void load_descriptor(void)
{
    OtaDescriptor_t primary;
    OtaDescriptor_t mirror;
    bool primary_ok = BSP_SPIFlash_IsAvailable() && read_descriptor_at(SPI_FLASH_OTA_META_BASE, &primary);
    bool mirror_ok = BSP_SPIFlash_IsAvailable() && read_descriptor_at(SPI_FLASH_OTA_META_MIRROR_BASE, &mirror);

    if (primary_ok && mirror_ok) {
        s_desc = (primary.transaction_id >= mirror.transaction_id) ? primary : mirror;
    } else if (primary_ok) {
        s_desc = primary;
    } else if (mirror_ok) {
        s_desc = mirror;
    } else {
        reset_descriptor();
    }
}

static bool ota_is_safe(void)
{
    ChargeCtrlView_t controller;
    CHG_LIB_SystemSummary_t summary;
    BMS_View_t bms;

    ChargeController_GetView(&controller);
    CHG_LIB_GetSystemSummary(&summary);
    BMS_GetView(&bms);

    /* Critical safety interlocks: block if charging, faulted, or in emergency stop */
    if (controller.running || controller.relay_should_close ||
        controller.faulted || controller.emergency_stop) {
        return false;
    }
    if (controller.state == CHARGE_CTRL_STATE_RUNNING ||
        controller.state == CHARGE_CTRL_STATE_PRECHARGE ||
        controller.state == CHARGE_CTRL_STATE_STOPPING ||
        controller.state == CHARGE_CTRL_STATE_FAULT) {
        return false;
    }
    if (!isfinite(summary.total_current) || fabsf(summary.total_current) > OTA_CURRENT_SAFE_A) {
        return false;
    }
    if (bms.online && bms.charge_relay_closed) {
        return false;
    }
    return true;
}

static void fail_ota(OtaStatusCode_t status)
{
    LOG("OTA: FAILED in step %d with status %d\r\n", (int)s_step, (int)status);
    /* Clean up modem HTTP transport and purge stale bytes */
    BSP_Quectel_SendCmd("AT+QHTTPSTOP");
    BSP_Quectel_ClearRx();
    QuectelEngine_SetOtaExclusive(false);
    s_desc.status = status;
    s_desc.boot_request = OTA_BOOT_FLAG_CLEARED;
    (void)save_descriptor();
    s_step = OTA_STEP_ERROR;
}

static bool json_string(const char *json, const char *key, char *out, size_t out_size)
{
    char needle[48];
    const char *start;
    const char *end;
    int written = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (written <= 0 || (size_t)written >= sizeof(needle)) return false;
    start = strstr(json, needle);
    if (start == NULL) return false;
    start = strchr(start + strlen(needle), ':');
    if (start == NULL) return false;
    start = strchr(start + 1, '\"');
    if (start == NULL) return false;
    ++start;
    end = strchr(start, '\"');
    if (end == NULL || (size_t)(end - start) >= out_size) return false;
    memcpy(out, start, (size_t)(end - start));
    out[end - start] = '\0';
    return true;
}

static bool json_u32(const char *json, const char *key, uint32_t *out, int base)
{
    char needle[48];
    const char *start;
    char *end;
    unsigned long value;
    int written = snprintf(needle, sizeof(needle), "\"%s\"", key);
    if (written <= 0 || (size_t)written >= sizeof(needle)) return false;
    start = strstr(json, needle);
    if (start == NULL) return false;
    start = strchr(start + strlen(needle), ':');
    if (start == NULL) return false;
    value = strtoul(start + 1, &end, base);
    if (end == start + 1 || value > 0xFFFFFFFFUL) return false;
    *out = (uint32_t)value;
    return true;
}

static bool parse_manifest(const char *json, uint32_t length,
                           uint32_t *version, uint32_t *size,
                           uint32_t *crc32, uint8_t sha256[OTA_SHA256_DIGEST_SIZE])
{
    char target[16];
    char filename[16];
    char sha_text[OTA_SHA256_DIGEST_SIZE * 2U + 1U];
    char crc_text[16];
    if (json == NULL || length == 0U || length >= sizeof(s_manifest_buf) ||
        (!json_string(json, "target_mcu", target, sizeof(target)) &&
         !json_string(json, "target", target, sizeof(target))) ||
        !json_string(json, "filename", filename, sizeof(filename)) ||
        strcmp(target, "STM32G0B1") != 0 || strcmp(filename, "Charger.bin") != 0 ||
        !json_u32(json, "version_code", version, 10) ||
        !json_u32(json, "size", size, 10) ||
        (!json_string(json, "crc32", crc_text, sizeof(crc_text)) &&
         !json_string(json, "crc32_hex", crc_text, sizeof(crc_text))) ||
        *size == 0U || *size > OTA_MAX_IMAGE_SIZE) {
        return false;
    }
    *crc32 = (uint32_t)strtoul(crc_text, NULL, 0);
    if (json_string(json, "sha256", sha_text, sizeof(sha_text)) &&
        strlen(sha_text) == OTA_SHA256_DIGEST_SIZE * 2U) {
        for (uint32_t i = 0U; i < OTA_SHA256_DIGEST_SIZE; ++i) {
            char pair[3] = { sha_text[i * 2U], sha_text[i * 2U + 1U], '\0' };
            char *end;
            unsigned long value = strtoul(pair, &end, 16);
            if (*end != '\0' || value > 0xFFUL) return false;
            sha256[i] = (uint8_t)value;
        }
    } else {
        memset(sha256, 0, OTA_SHA256_DIGEST_SIZE);
    }
    return true;
}

static bool make_firmware_url(const char *manifest_url, char *firmware_url, size_t size)
{
    const char *slash = strrchr(manifest_url, '/');
    size_t prefix_len;
    if (slash == NULL) return false;
    prefix_len = (size_t)(slash - manifest_url);
    if (prefix_len + strlen("/firmware") + 1U > size) return false;
    memcpy(firmware_url, manifest_url, prefix_len);
    memcpy(firmware_url + prefix_len, "/firmware", strlen("/firmware") + 1U);
    return true;
}

void OTAService_Init(void)
{
    load_descriptor();
    s_step = OTA_STEP_IDLE;
    s_manifest_mode = false;
    s_policy_enabled = (s_desc.policy_enabled == 1U && s_desc.policy_manifest_url[0] != '\0');
    s_policy_interval_ms = (s_desc.policy_interval_ms == 0U)
        ? OTA_DEFAULT_CHECK_INTERVAL_MS : s_desc.policy_interval_ms;
    if (s_policy_enabled && strncmp(s_desc.policy_manifest_url, "https://", 8U) == 0) {
        memcpy(s_policy_manifest_url, s_desc.policy_manifest_url, sizeof(s_policy_manifest_url));
        s_policy_manifest_url[sizeof(s_policy_manifest_url) - 1U] = '\0';
    } else {
        s_policy_enabled = false;
        s_policy_manifest_url[0] = '\0';
    }
    LOG("OTA_Service: Initialized (Status=%lu, BootReq=0x%08lX)\r\n",
        (unsigned long)s_desc.status, (unsigned long)s_desc.boot_request);
}

bool OTAService_StartDownload(const char *url, uint32_t version,
                              uint32_t expected_size, uint32_t expected_crc32,
                              const uint8_t expected_sha256[OTA_SHA256_DIGEST_SIZE])
{
    size_t url_len;
    if (url == NULL || expected_sha256 == NULL || expected_size == 0U ||
        expected_size > OTA_MAX_IMAGE_SIZE || !BSP_SPIFlash_IsAvailable() ||
        !ota_is_safe() ||
        (s_step != OTA_STEP_IDLE && !(s_manifest_mode && s_step == OTA_STEP_WAIT_READ_DONE))) {
        return false;
    }
    url_len = strlen(url);
    if (url_len == 0U || url_len > OTA_URL_MAX_LENGTH ||
        (strncmp(url, "https://", 8U) != 0)) {
        return false;
    }
    if (!BSP_Quectel_IsReady() || !QuectelEngine_IsNetReady()) return false;

    bool policy_enabled = s_policy_enabled;
    uint32_t policy_interval_ms = s_policy_interval_ms;
    char policy_url[sizeof(s_policy_manifest_url)];
    memcpy(policy_url, s_policy_manifest_url, sizeof(policy_url));
    memcpy(s_url, url, url_len + 1U);
    s_manifest_mode = false;
    reset_descriptor();
    s_desc.policy_enabled = policy_enabled ? 1U : 0U;
    s_desc.policy_interval_ms = policy_interval_ms;
    memcpy(s_desc.policy_manifest_url, policy_url, sizeof(s_desc.policy_manifest_url));
    s_desc.version = version;
    s_desc.image_size = expected_size;
    s_desc.image_crc32 = expected_crc32;
    memcpy(s_desc.expected_sha256, expected_sha256, sizeof(s_desc.expected_sha256));
    s_desc.status = OTA_STATUS_DOWNLOADING;
    s_desc.transaction_id = 1U;
    s_desc.boot_request = OTA_BOOT_FLAG_CLEARED;
    s_desc.downloaded_bytes = 0U;
    s_desc.calc_crc32 = 0U;
    s_crc_state = 0xFFFFFFFFU;
    OTA_SHA256_Init(&s_sha256);
    s_read_expected_bytes = expected_size;
    if (!save_descriptor()) return false;

    QuectelEngine_SetOtaExclusive(true);
    BSP_Quectel_ClearRx();
    s_erase_index = 0U;
    s_erase_count = (expected_size + SPI_FLASH_SECTOR_SIZE - 1U) / SPI_FLASH_SECTOR_SIZE;
    s_step = OTA_STEP_ERASE_START;
    s_step_tick = HAL_GetTick();
    return true;
}

bool OTAService_StartManifestCheck(const char *manifest_url)
{
    size_t url_len;
    /* Clean recovery if previous attempt was in error state */
    if (s_step == OTA_STEP_ERROR) {
        QuectelEngine_SetOtaExclusive(false);
        s_step = OTA_STEP_IDLE;
    }

    if (manifest_url == NULL || !ota_is_safe() || !BSP_SPIFlash_IsAvailable() ||
        !BSP_Quectel_IsReady() || !QuectelEngine_IsNetReady() || s_step != OTA_STEP_IDLE) return false;
    url_len = strlen(manifest_url);
    if (url_len == 0U || url_len > OTA_URL_MAX_LENGTH ||
        strncmp(manifest_url, "https://", 8U) != 0) return false;
    memcpy(s_url, manifest_url, url_len + 1U);
    QuectelEngine_SetOtaExclusive(true);
    BSP_Quectel_ClearRx();
    s_manifest_mode = true;
    s_manifest_received = 0U;
    s_read_expected_bytes = 0U;
    s_desc.status = OTA_STATUS_DOWNLOADING;
    s_step = OTA_STEP_CHECK_PDP;   /* Verify PDP context before QHTTP */
    s_step_tick = HAL_GetTick();
    LOG("OTA: StartManifestCheck -> CHECK_PDP, URL=%s\r\n", manifest_url);
    return true;
}

bool OTAService_SetPolicy(bool enabled, uint32_t interval_ms, const char *manifest_url)
{
    size_t length = 0U;
    if (enabled) {
        if (manifest_url == NULL || strncmp(manifest_url, "https://", 8U) != 0) return false;
        length = strlen(manifest_url);
        if (length == 0U || length > OTA_URL_MAX_LENGTH) return false;
    }
    bool old_enabled = s_policy_enabled;
    uint32_t old_interval = s_policy_interval_ms;
    char old_url[sizeof(s_policy_manifest_url)];
    memcpy(old_url, s_policy_manifest_url, sizeof(old_url));

    /* Preserve descriptor fields for full rollback */
    uint32_t old_desc_enabled = s_desc.policy_enabled;
    uint32_t old_desc_interval = s_desc.policy_interval_ms;
    char old_desc_url[sizeof(s_desc.policy_manifest_url)];
    memcpy(old_desc_url, s_desc.policy_manifest_url, sizeof(old_desc_url));

    s_policy_enabled = enabled;
    s_policy_interval_ms = (interval_ms == 0U) ? OTA_DEFAULT_CHECK_INTERVAL_MS : interval_ms;
    if (enabled) memcpy(s_policy_manifest_url, manifest_url, length + 1U);
    else s_policy_manifest_url[0] = '\0';
    s_desc.policy_enabled = enabled ? 1U : 0U;
    s_desc.policy_interval_ms = s_policy_interval_ms;
    memset(s_desc.policy_manifest_url, 0, sizeof(s_desc.policy_manifest_url));
    memcpy(s_desc.policy_manifest_url, s_policy_manifest_url, strlen(s_policy_manifest_url) + 1U);
    s_next_policy_tick = HAL_GetTick();
    if (save_descriptor()) return true;

    /* Rollback both runtime variables and persistent descriptor state */
    s_policy_enabled = old_enabled;
    s_policy_interval_ms = old_interval;
    memcpy(s_policy_manifest_url, old_url, sizeof(old_url));

    s_desc.policy_enabled = old_desc_enabled;
    s_desc.policy_interval_ms = old_desc_interval;
    memcpy(s_desc.policy_manifest_url, old_desc_url, sizeof(s_desc.policy_manifest_url));
    return false;
}

OtaCheckResult_t OTAService_RequestCheckNowResult(void)
{
    if (s_step == OTA_STEP_ERROR) {
        QuectelEngine_SetOtaExclusive(false);
        s_step = OTA_STEP_IDLE;
    }
    if (!s_policy_enabled || s_policy_manifest_url[0] == '\0') {
        return OTA_CHECK_ERR_POLICY_DISABLED;
    }
    if (strncmp(s_policy_manifest_url, "https://", 8U) != 0) {
        return OTA_CHECK_ERR_INVALID_URL;
    }
    if (!ota_is_safe()) {
        return OTA_CHECK_ERR_NOT_SAFE;
    }
    if (!BSP_SPIFlash_IsAvailable()) {
        return OTA_CHECK_ERR_FLASH_BUSY;
    }
    if (!BSP_Quectel_IsReady() || !QuectelEngine_IsNetReady()) {
        return OTA_CHECK_ERR_NET_NOT_READY;
    }
    if (s_step != OTA_STEP_IDLE) {
        return OTA_CHECK_ERR_BUSY;
    }
    if (OTAService_StartManifestCheck(s_policy_manifest_url)) {
        return OTA_CHECK_OK;
    }
    return OTA_CHECK_ERR_BUSY;
}

bool OTAService_RequestCheckNow(void)
{
    return (OTAService_RequestCheckNowResult() == OTA_CHECK_OK);
}

void OTAService_GetStatus(OtaStatusView_t *out_status)
{
    if (out_status == NULL) return;
    out_status->status = s_desc.status;
    out_status->version = s_desc.version;
    out_status->image_size = s_desc.image_size;
    out_status->downloaded_bytes = s_desc.downloaded_bytes;
    out_status->boot_request = s_desc.boot_request;
    out_status->boot_attempts = s_desc.boot_attempts;
    out_status->policy_enabled = s_policy_enabled ? 1U : 0U;
}

void OTAService_Abort(void)
{
    bool policy_enabled = s_policy_enabled;
    uint32_t policy_interval_ms = s_policy_interval_ms;
    char policy_url[sizeof(s_policy_manifest_url)];
    memcpy(policy_url, s_policy_manifest_url, sizeof(policy_url));

    /* Send abort to modem and flush stale bytes */
    BSP_Quectel_SendCmd("AT+QHTTPSTOP");
    BSP_Quectel_ClearRx();
    QuectelEngine_SetOtaExclusive(false);
    s_step = OTA_STEP_IDLE;
    reset_descriptor();
    s_desc.policy_enabled = policy_enabled ? 1U : 0U;
    s_desc.policy_interval_ms = policy_interval_ms;
    memcpy(s_desc.policy_manifest_url, policy_url, sizeof(s_desc.policy_manifest_url));
    (void)save_descriptor();
}

void OTAService_GetDescriptor(OtaDescriptor_t *out_desc)
{
    if (out_desc != NULL) memcpy(out_desc, &s_desc, sizeof(*out_desc));
}

bool OTAService_RequestApply(void)
{
    if (s_step != OTA_STEP_IDLE || s_desc.status != OTA_STATUS_VERIFIED || !ota_is_safe()) {
        return false;
    }
    s_desc.boot_request = OTA_BOOT_FLAG_REQUEST;
    s_desc.status = OTA_STATUS_VERIFIED;
    s_desc.boot_attempts = 0U;
    s_desc.health_marker = OTA_HEALTH_MARKER_PENDING;
    return save_descriptor();
}

void OTAService_ConfirmBoot(void)
{
    if (s_desc.status == OTA_STATUS_BOOT_TEST &&
        s_desc.boot_request == OTA_BOOT_FLAG_CLEARED) {
        s_desc.health_marker = OTA_HEALTH_MARKER_CONFIRMED;
        s_desc.status = OTA_STATUS_APPLIED;
        s_desc.boot_attempts = 0U;
        (void)save_descriptor();
    }
}

void OTAService_Process(uint32_t now_tick)
{
    char line[256];

    if (s_step == OTA_STEP_IDLE && s_policy_enabled && s_policy_manifest_url[0] != '\0' &&
        (uint32_t)(now_tick - s_next_policy_tick) >= s_policy_interval_ms) {
        s_next_policy_tick = now_tick;
        (void)OTAService_RequestCheckNow();
    }

    switch (s_step) {
    case OTA_STEP_IDLE:
        break;

    case OTA_STEP_ERASE_START:
        if (s_erase_index >= s_erase_count) {
            LOG("OTA: Erase completed %lu sectors, verifying PDP context...\r\n", s_erase_count);
            s_step = OTA_STEP_CHECK_PDP;
            s_step_tick = now_tick;
            break;
        }
        s_step_tick = now_tick;
        if (!BSP_SPIFlash_StartEraseSector4K(SPI_FLASH_OTA_STAGING_BASE +
                                             s_erase_index * SPI_FLASH_SECTOR_SIZE)) {
            fail_ota(OTA_STATUS_ERROR_FLASH);
            break;
        }
        s_step = OTA_STEP_ERASE_WAIT;
        break;

    case OTA_STEP_ERASE_WAIT:
        if (BSP_SPIFlash_IsBusy()) {
            if ((uint32_t)(now_tick - s_step_tick) > OTA_ERASE_TIMEOUT_MS) {
                LOG("OTA: Erase timeout sector %lu\r\n", s_erase_index);
                fail_ota(OTA_STATUS_ERROR_TIMEOUT);
            }
            break; /* Yield execution back to main loop to feed IWDG */
        }
        /* Sector erase complete, advance to next sector */
        ++s_erase_index;
        s_step = OTA_STEP_ERASE_START;
        s_step_tick = now_tick;
        break;

    /* ── PDP context verification (manifest & firmware download) ─────── */
    case OTA_STEP_CHECK_PDP:
        LOG("OTA: CHECK_PDP -> AT+QIACT?\r\n");
        if (!BSP_Quectel_SendCmd("AT+QIACT?")) {
            fail_ota(OTA_STATUS_ERROR_NETWORK);
        } else {
            s_step = OTA_STEP_WAIT_CHECK_PDP;
            s_step_tick = now_tick;
        }
        break;

    case OTA_STEP_WAIT_CHECK_PDP: {
        /* +QIACT: 1,1,1,"10.x.x.x" → PDP active, proceed
         * OK without +QIACT line    → PDP not active, re-activate */
        static bool pdp_found;
        if (s_step_tick == now_tick) pdp_found = false; /* Reset on entry */
        if (BSP_Quectel_ReadLine(line, sizeof(line))) {
            if (line[0] == '\0') break;
            if (strncmp(line, "+QIACT:", 7) == 0) {
                pdp_found = true;
                LOG("OTA: PDP confirmed active: %s\r\n", line);
            } else if (strcmp(line, "OK") == 0) {
                if (pdp_found) {
                    s_step = OTA_STEP_CFG_HTTP_CTX;
                    LOG("OTA: PDP OK -> CFG_HTTP_CTX\r\n");
                } else {
                    LOG("OTA: PDP not active -> ACTIVATE_PDP\r\n");
                    s_step = OTA_STEP_ACTIVATE_PDP;
                }
                s_step_tick = now_tick;
            } else if (strstr(line, "ERROR") != NULL) {
                LOG("OTA: QIACT? ERROR -> ACTIVATE_PDP\r\n");
                s_step = OTA_STEP_ACTIVATE_PDP;
                s_step_tick = now_tick;
            }
        } else if ((uint32_t)(now_tick - s_step_tick) >= OTA_AT_TIMEOUT_MS) {
            LOG("OTA: WAIT_CHECK_PDP timeout\r\n");
            fail_ota(OTA_STATUS_ERROR_TIMEOUT);
        }
        break;
    }

    case OTA_STEP_ACTIVATE_PDP:
        LOG("OTA: ACTIVATE_PDP -> AT+QIACT=1\r\n");
        if (!BSP_Quectel_SendCmd("AT+QIACT=1")) {
            fail_ota(OTA_STATUS_ERROR_NETWORK);
        } else {
            s_step = OTA_STEP_WAIT_ACTIVATE_PDP;
            s_step_tick = now_tick;
        }
        break;

    case OTA_STEP_WAIT_ACTIVATE_PDP:
        if (BSP_Quectel_ReadLine(line, sizeof(line))) {
            if (line[0] == '\0') break;
            if (strcmp(line, "OK") == 0) {
                LOG("OTA: PDP re-activated -> CFG_HTTP_CTX\r\n");
                s_step = OTA_STEP_CFG_HTTP_CTX;
                s_step_tick = now_tick;
            } else if (strstr(line, "ERROR") != NULL) {
                /* Already active (CME ERROR: 148) is acceptable */
                LOG("OTA: QIACT=1 response: %s (treating as active)\r\n", line);
                s_step = OTA_STEP_CFG_HTTP_CTX;
                s_step_tick = now_tick;
            }
        } else if ((uint32_t)(now_tick - s_step_tick) >= 20000U) {
            /* PDP activation can take up to 15s on first attach */
            LOG("OTA: WAIT_ACTIVATE_PDP timeout\r\n");
            fail_ota(OTA_STATUS_ERROR_TIMEOUT);
        }
        break;

    /* ── HTTP configuration steps ─────────────────────────────────────── */
    case OTA_STEP_CFG_HTTP_CTX:
        LOG("OTA: CFG_HTTP_CTX -> AT+QHTTPCFG=\"contextid\",1\r\n");
        if (!BSP_Quectel_SendCmd("AT+QHTTPCFG=\"contextid\",1")) {
            fail_ota(OTA_STATUS_ERROR_NETWORK);
        } else {
            s_step = OTA_STEP_WAIT_CFG_HTTP_CTX;
            s_step_tick = now_tick;
        }
        break;

    case OTA_STEP_WAIT_CFG_HTTP_CTX:
        if (BSP_Quectel_ReadLine(line, sizeof(line))) {
            if (line[0] == '\0') break; /* Skip empty lines / unsolicited URCs */
            if (strcmp(line, "OK") == 0 || strstr(line, "ERROR") != NULL) {
                LOG("OTA: QHTTPCFG contextid -> %s\r\n", line);
                s_step = OTA_STEP_CFG_HTTP_SSL;
                s_step_tick = now_tick;
            }
        } else if ((uint32_t)(now_tick - s_step_tick) >= OTA_AT_TIMEOUT_MS) {
            LOG("OTA: WAIT_CFG_HTTP_CTX timeout\r\n");
            fail_ota(OTA_STATUS_ERROR_TIMEOUT);
        }
        break;

    case OTA_STEP_CFG_HTTP_SSL:
        LOG("OTA: CFG_HTTP_SSL -> AT+QHTTPCFG=\"sslctxid\",1\r\n");
        if (!BSP_Quectel_SendCmd("AT+QHTTPCFG=\"sslctxid\",1")) {
            fail_ota(OTA_STATUS_ERROR_NETWORK);
        } else {
            s_step = OTA_STEP_WAIT_CFG_HTTP_SSL;
            s_step_tick = now_tick;
        }
        break;

    case OTA_STEP_WAIT_CFG_HTTP_SSL:
        if (BSP_Quectel_ReadLine(line, sizeof(line))) {
            if (line[0] == '\0') break; /* Skip empty lines / unsolicited URCs */
            if (strcmp(line, "OK") == 0 || strstr(line, "ERROR") != NULL) {
                LOG("OTA: QHTTPCFG sslctxid -> %s\r\n", line);
                s_step = OTA_STEP_CFG_SSL_SNI;
                s_step_tick = now_tick;
            }
        } else if ((uint32_t)(now_tick - s_step_tick) >= OTA_AT_TIMEOUT_MS) {
            LOG("OTA: WAIT_CFG_HTTP_SSL timeout\r\n");
            fail_ota(OTA_STATUS_ERROR_TIMEOUT);
        }
        break;

    case OTA_STEP_CFG_SSL_SNI:
        LOG("OTA: CFG_SSL_SNI -> AT+QSSLCFG=\"sni\",1,1\r\n");
        if (!BSP_Quectel_SendCmd("AT+QSSLCFG=\"sni\",1,1")) {
            fail_ota(OTA_STATUS_ERROR_NETWORK);
        } else {
            s_step = OTA_STEP_WAIT_CFG_SSL_SNI;
            s_step_tick = now_tick;
        }
        break;

    case OTA_STEP_WAIT_CFG_SSL_SNI:
        if (BSP_Quectel_ReadLine(line, sizeof(line))) {
            if (line[0] == '\0') break; /* Skip empty lines / unsolicited URCs */
            if (strcmp(line, "OK") == 0 || strstr(line, "ERROR") != NULL) {
                LOG("OTA: QSSLCFG sni -> %s\r\n", line);
                s_step = OTA_STEP_CFG_SSL_SEC;
                s_step_tick = now_tick;
            }
        } else if ((uint32_t)(now_tick - s_step_tick) >= OTA_AT_TIMEOUT_MS) {
            LOG("OTA: WAIT_CFG_SSL_SNI timeout\r\n");
            fail_ota(OTA_STATUS_ERROR_TIMEOUT);
        }
        break;

    case OTA_STEP_CFG_SSL_SEC:
        LOG("OTA: CFG_SSL_SEC -> AT+QSSLCFG=\"seclevel\",1,0\r\n");
        if (!BSP_Quectel_SendCmd("AT+QSSLCFG=\"seclevel\",1,0")) {
            fail_ota(OTA_STATUS_ERROR_NETWORK);
        } else {
            s_step = OTA_STEP_WAIT_CFG_SSL_SEC;
            s_step_tick = now_tick;
        }
        break;

    case OTA_STEP_WAIT_CFG_SSL_SEC:
        if (BSP_Quectel_ReadLine(line, sizeof(line))) {
            if (line[0] == '\0') break; /* Skip empty lines / unsolicited URCs */
            if (strcmp(line, "OK") == 0 || strstr(line, "ERROR") != NULL) {
                LOG("OTA: QSSLCFG seclevel -> %s\r\n", line);
                s_step = OTA_STEP_SET_URL_CMD;
                s_step_tick = now_tick;
            }
        } else if ((uint32_t)(now_tick - s_step_tick) >= OTA_AT_TIMEOUT_MS) {
            LOG("OTA: WAIT_CFG_SSL_SEC timeout\r\n");
            fail_ota(OTA_STATUS_ERROR_TIMEOUT);
        }
        break;

    case OTA_STEP_SET_URL_CMD: {
        char cmd[32];
        int written = snprintf(cmd, sizeof(cmd), "AT+QHTTPURL=%u,30", (unsigned)strlen(s_url));
        LOG("OTA: SET_URL_CMD -> %s\r\n", cmd);
        if (written <= 0 || (size_t)written >= sizeof(cmd) || !BSP_Quectel_SendCmd(cmd)) {
            fail_ota(OTA_STATUS_ERROR_NETWORK);
        } else {
            s_step = OTA_STEP_WAIT_CONNECT_URL;
            s_step_tick = now_tick;
        }
        break;
    }

    case OTA_STEP_WAIT_CONNECT_URL:
        if (BSP_Quectel_ReadLine(line, sizeof(line))) {
            if (line[0] == '\0') break; /* Skip empty lines / unsolicited URCs */
            if (strcmp(line, "CONNECT") == 0) {
                LOG("OTA: CONNECT received, sending URL body\r\n");
                if (!BSP_Quectel_SendString(s_url)) fail_ota(OTA_STATUS_ERROR_NETWORK);
                else {
                    s_step = OTA_STEP_SEND_URL_BODY;
                    s_step_tick = now_tick;
                }
            } else if (strstr(line, "ERROR") != NULL) {
                LOG("OTA: WAIT_CONNECT_URL ERROR: %s\r\n", line);
                fail_ota(OTA_STATUS_ERROR_NETWORK);
            }
        } else if ((uint32_t)(now_tick - s_step_tick) >= OTA_AT_TIMEOUT_MS) {
            LOG("OTA: WAIT_CONNECT_URL timeout\r\n");
            fail_ota(OTA_STATUS_ERROR_TIMEOUT);
        }
        break;

    case OTA_STEP_SEND_URL_BODY:
        if (BSP_Quectel_ReadLine(line, sizeof(line))) {
            if (line[0] == '\0') break; /* Skip empty lines between AT responses */
            if (strcmp(line, "OK") == 0) {
                LOG("OTA: URL accepted, sending AT+QHTTPGET=60\r\n");
                if (!BSP_Quectel_SendCmd("AT+QHTTPGET=60")) fail_ota(OTA_STATUS_ERROR_NETWORK);
                else {
                    s_step = OTA_STEP_WAIT_GET_RESP;
                    s_step_tick = now_tick;
                }
            } else if (strstr(line, "ERROR") != NULL) {
                LOG("OTA: SEND_URL_BODY ERROR: %s\r\n", line);
                fail_ota(OTA_STATUS_ERROR_NETWORK);
            }
        } else if ((uint32_t)(now_tick - s_step_tick) >= OTA_AT_TIMEOUT_MS) {
            LOG("OTA: SEND_URL_BODY timeout\r\n");
            fail_ota(OTA_STATUS_ERROR_TIMEOUT);
        }
        break;

    case OTA_STEP_WAIT_GET_RESP:
        if (BSP_Quectel_ReadLine(line, sizeof(line))) {
            if (line[0] == '\0') break; /* Skip empty lines / unsolicited URCs */
            if (strncmp(line, "+QHTTPGET:", 10U) == 0) {
                int err = -1;
                int status_code = 0;
                unsigned content_len = 0U;
                int fields = sscanf(line + 10, "%d,%d,%u", &err, &status_code, &content_len);
                LOG("OTA: +QHTTPGET: err=%d code=%d len=%u\r\n", err, status_code, content_len);
                if (fields >= 3 && err == 0 && status_code == 200 &&
                    content_len > 0U &&
                    (s_manifest_mode ? content_len < sizeof(s_manifest_buf)
                                     : content_len == s_desc.image_size)) {
                    s_read_expected_bytes = s_manifest_mode ? content_len : s_desc.image_size;
                    LOG("OTA: QHTTPGET 200 OK, requesting QHTTPREAD=120 for %lu bytes\r\n",
                        (unsigned long)s_read_expected_bytes);
                    if (!BSP_Quectel_SendCmd("AT+QHTTPREAD=120")) fail_ota(OTA_STATUS_ERROR_NETWORK);
                    else {
                        s_step = OTA_STEP_WAIT_READ_CONNECT;
                        s_step_tick = now_tick;
                    }
                } else {
                    fail_ota(OTA_STATUS_ERROR_NETWORK);
                }
            } else if (strstr(line, "ERROR") != NULL) {
                fail_ota(OTA_STATUS_ERROR_NETWORK);
            }
        } else if ((uint32_t)(now_tick - s_step_tick) >= OTA_HTTP_TIMEOUT_MS) {
            LOG("OTA: WAIT_GET_RESP timeout\r\n");
            fail_ota(OTA_STATUS_ERROR_TIMEOUT);
        }
        break;

    case OTA_STEP_WAIT_READ_CONNECT:
        if (BSP_Quectel_ReadLine(line, sizeof(line))) {
            if (line[0] == '\0') break; /* Skip empty lines / unsolicited URCs */
            if (strcmp(line, "CONNECT") == 0) {
                LOG("OTA: QHTTPREAD CONNECT -> READING_STREAM, expect %lu bytes\r\n",
                    (unsigned long)s_read_expected_bytes);
                s_step = OTA_STEP_READING_STREAM;
                s_step_tick = now_tick;
            } else if (strstr(line, "ERROR") != NULL) {
                LOG("OTA: WAIT_READ_CONNECT ERROR: %s\r\n", line);
                fail_ota(OTA_STATUS_ERROR_NETWORK);
            }
        } else if ((uint32_t)(now_tick - s_step_tick) >= OTA_HTTP_TIMEOUT_MS) {
            LOG("OTA: WAIT_READ_CONNECT timeout\r\n");
            fail_ota(OTA_STATUS_ERROR_TIMEOUT);
        }
        break;

    case OTA_STEP_READING_STREAM: {
        uint8_t chunk[OTA_STREAM_CHUNK_SIZE];
        uint32_t received_total = s_manifest_mode ? s_manifest_received : s_desc.downloaded_bytes;
        uint32_t remaining = s_read_expected_bytes - received_total;
        uint16_t wanted = (remaining > sizeof(chunk)) ? sizeof(chunk) : (uint16_t)remaining;
        uint16_t received = BSP_Quectel_Read(chunk, wanted);
        if (received > 0U) {
            if (s_manifest_mode) {
                memcpy(&s_manifest_buf[s_manifest_received], chunk, received);
                s_manifest_received += received;
            } else {
                uint32_t address = SPI_FLASH_OTA_STAGING_BASE + s_desc.downloaded_bytes;
                if (!BSP_SPIFlash_Write(address, chunk, received)) {
                    LOG("OTA: Flash write failed at 0x%08lX\r\n", (unsigned long)address);
                    fail_ota(OTA_STATUS_ERROR_FLASH);
                    break;
                }
                s_crc_state = crc32_update(s_crc_state, chunk, received);
                OTA_SHA256_Update(&s_sha256, chunk, received);
                s_desc.downloaded_bytes += received;

                if (s_desc.downloaded_bytes % 10240U < received) {
                    LOG("OTA: Download progress: %lu / %lu bytes\r\n",
                        (unsigned long)s_desc.downloaded_bytes, (unsigned long)s_read_expected_bytes);
                }
            }
            s_step_tick = now_tick;
            if ((s_manifest_mode ? s_manifest_received : s_desc.downloaded_bytes) == s_read_expected_bytes) {
                if (s_manifest_mode) {
                    LOG("OTA: Manifest read complete (%lu bytes), waiting for OK\r\n",
                        (unsigned long)s_read_expected_bytes);
                    s_step = OTA_STEP_WAIT_READ_DONE;
                    s_step_tick = now_tick;
                } else {
                    LOG("OTA: Firmware stream complete (%lu bytes), proceeding directly to VERIFY\r\n",
                        (unsigned long)s_desc.downloaded_bytes);
                    s_step = OTA_STEP_VERIFY;
                    s_step_tick = now_tick;
                }
            }
        } else if ((uint32_t)(now_tick - s_step_tick) >= OTA_BODY_TIMEOUT_MS) {
            LOG("OTA: READING_STREAM stall timeout\r\n");
            fail_ota(OTA_STATUS_ERROR_TIMEOUT);
        }
        break;
    }

    case OTA_STEP_WAIT_READ_DONE:
        if (BSP_Quectel_ReadLine(line, sizeof(line))) {
            if (line[0] == '\0') break; /* Skip empty lines / unsolicited URCs */
            if (strcmp(line, "OK") == 0 || strncmp(line, "+QHTTPREAD: 0", 13) == 0) {
                LOG("OTA: QHTTPREAD complete (line: %s)\r\n", line);
                if (s_manifest_mode) {
                    uint32_t version;
                    uint32_t size;
                    uint32_t crc32;
                    uint8_t sha256[OTA_SHA256_DIGEST_SIZE];
                    char firmware_url[OTA_URL_MAX_LENGTH + 1U];
                    s_manifest_buf[s_manifest_received] = '\0';
                    if (!parse_manifest((const char *)s_manifest_buf, s_manifest_received,
                                        &version, &size, &crc32, sha256) ||
                        !make_firmware_url(s_url, firmware_url, sizeof(firmware_url))) {
                        fail_ota(OTA_STATUS_ERROR_NETWORK);
                    } else {
                        if (!OTAService_StartDownload(firmware_url, version, size, crc32, sha256)) {
                            fail_ota(OTA_STATUS_ERROR_NETWORK);
                        }
                    }
                } else {
                    LOG("OTA: Firmware download complete, entering VERIFY\r\n");
                    s_step = OTA_STEP_VERIFY;
                }
            }
            else if (strstr(line, "ERROR") != NULL) {
                LOG("OTA: WAIT_READ_DONE ERROR: %s\r\n", line);
                fail_ota(OTA_STATUS_ERROR_NETWORK);
            }
        } else if ((uint32_t)(now_tick - s_step_tick) >= 30000U) {
            LOG("OTA: WAIT_READ_DONE timeout\r\n");
            fail_ota(OTA_STATUS_ERROR_TIMEOUT);
        }
        break;

    case OTA_STEP_VERIFY: {
        bool sha_match = true;
        bool has_expected_sha = false;
        s_desc.calc_crc32 = s_crc_state ^ 0xFFFFFFFFU;
        OTA_SHA256_Final(&s_sha256, s_desc.calc_sha256);
        for (uint32_t i = 0U; i < OTA_SHA256_DIGEST_SIZE; ++i) {
            if (s_desc.expected_sha256[i] != 0U) {
                has_expected_sha = true;
                break;
            }
        }
        if (has_expected_sha) {
            sha_match = (memcmp(s_desc.calc_sha256, s_desc.expected_sha256,
                                sizeof(s_desc.expected_sha256)) == 0);
        }
        if (s_desc.downloaded_bytes != s_desc.image_size) {
            fail_ota(OTA_STATUS_ERROR_SIZE);
        } else if (s_desc.calc_crc32 != s_desc.image_crc32 || !sha_match) {
            fail_ota(OTA_STATUS_ERROR_CRC);
        } else {
            s_desc.status = OTA_STATUS_VERIFIED;
            s_desc.boot_request = OTA_BOOT_FLAG_CLEARED;
            if (!save_descriptor()) fail_ota(OTA_STATUS_ERROR_FLASH);
            else {
                BSP_Quectel_SendCmd("AT+QHTTPSTOP");
                BSP_Quectel_ClearRx();
                QuectelEngine_SetOtaExclusive(false);
                s_step = OTA_STEP_IDLE;
                LOG("OTA: VERIFY SUCCESS! Image valid (size=%lu, crc32=0x%08lX)\r\n",
                    (unsigned long)s_desc.image_size, (unsigned long)s_desc.calc_crc32);
            }
        }
        break;
    }

    case OTA_STEP_ERROR:
        s_step = OTA_STEP_IDLE;
        break;

    default:
        fail_ota(OTA_STATUS_ERROR_NETWORK);
        break;
    }
}

bool OTAService_SelfTestFlash(uint32_t *out_jedec, uint32_t *out_cap_kb)
{
    return BSP_SPIFlash_SelfTest(out_jedec, out_cap_kb);
}

typedef struct {
    bool active;
    uint32_t total_size;
    uint32_t expected_crc32;
    uint32_t version;
    uint32_t received_bytes;
} DirectUploadState_t;

static DirectUploadState_t s_direct_upload;

bool OTAService_DirectUploadStart(uint32_t total_size, uint32_t expected_crc32, uint32_t version)
{
    if (!ota_is_safe() || !BSP_SPIFlash_IsAvailable()) {
        return false;
    }
    if (total_size == 0U || total_size > OTA_MAX_IMAGE_SIZE) {
        return false;
    }
    if (s_step != OTA_STEP_IDLE) {
        return false;
    }

    uint32_t sectors = (total_size + SPI_FLASH_SECTOR_SIZE - 1U) / SPI_FLASH_SECTOR_SIZE;
    for (uint32_t i = 0U; i < sectors; ++i) {
        if (!BSP_SPIFlash_EraseSector4K(SPI_FLASH_OTA_STAGING_BASE + i * SPI_FLASH_SECTOR_SIZE)) {
            return false;
        }
    }

    s_direct_upload.active = true;
    s_direct_upload.total_size = total_size;
    s_direct_upload.expected_crc32 = expected_crc32;
    s_direct_upload.version = version;
    s_direct_upload.received_bytes = 0U;

    reset_descriptor();
    s_desc.image_size = total_size;
    s_desc.image_crc32 = expected_crc32;
    s_desc.version = version;
    s_desc.status = OTA_STATUS_DOWNLOADING;
    s_desc.downloaded_bytes = 0U;

    return true;
}

bool OTAService_DirectUploadChunk(uint32_t offset, const uint8_t *data, uint16_t len)
{
    if (!s_direct_upload.active || data == NULL || len == 0U) {
        return false;
    }
    if ((offset + len) > s_direct_upload.total_size) {
        return false;
    }

    if (!BSP_SPIFlash_Write(SPI_FLASH_OTA_STAGING_BASE + offset, data, len)) {
        return false;
    }

    s_direct_upload.received_bytes += len;
    s_desc.downloaded_bytes = s_direct_upload.received_bytes;
    return true;
}

bool OTAService_DirectUploadFinish(void)
{
    if (!s_direct_upload.active) {
        return false;
    }
    if (s_direct_upload.received_bytes != s_direct_upload.total_size) {
        s_direct_upload.active = false;
        s_desc.status = OTA_STATUS_ERROR_SIZE;
        return false;
    }

    uint32_t calc_crc = 0xFFFFFFFFU;
    uint8_t buf[256];
    uint32_t remaining = s_direct_upload.total_size;
    uint32_t addr = SPI_FLASH_OTA_STAGING_BASE;
    while (remaining > 0U) {
        uint32_t chunk = (remaining > sizeof(buf)) ? sizeof(buf) : remaining;
        if (!BSP_SPIFlash_Read(addr, buf, chunk)) {
            s_direct_upload.active = false;
            s_desc.status = OTA_STATUS_ERROR_FLASH;
            return false;
        }
        calc_crc = crc32_update(calc_crc, buf, chunk);
        addr += chunk;
        remaining -= chunk;
    }
    calc_crc ^= 0xFFFFFFFFU;

    if (calc_crc != s_direct_upload.expected_crc32) {
        s_desc.status = OTA_STATUS_ERROR_CRC;
        s_direct_upload.active = false;
        return false;
    }

    s_desc.calc_crc32 = calc_crc;
    s_desc.status = OTA_STATUS_VERIFIED;
    s_desc.boot_request = OTA_BOOT_FLAG_REQUEST;
    s_desc.boot_attempts = 0U;
    s_desc.health_marker = OTA_HEALTH_MARKER_PENDING;
    s_direct_upload.active = false;

    return save_descriptor();
}
