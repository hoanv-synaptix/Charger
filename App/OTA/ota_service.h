/**
 * @file    ota_service.h
 * @brief   Application OTA Layer - Firmware Download & Verification Service
 */

#ifndef OTA_SERVICE_H
#define OTA_SERVICE_H

#include "ota_types.h"

#define OTA_SHA256_DIGEST_SIZE 32U

#ifdef __cplusplus
extern "C" {
#endif

void OTAService_Init(void);
void OTAService_Process(uint32_t now_tick);
void OTAService_ConfirmBoot(void);

typedef enum {
    OTA_CHECK_OK = 0,
    OTA_CHECK_ERR_POLICY_DISABLED,
    OTA_CHECK_ERR_NOT_SAFE,
    OTA_CHECK_ERR_FLASH_BUSY,
    OTA_CHECK_ERR_NET_NOT_READY,
    OTA_CHECK_ERR_BUSY,
    OTA_CHECK_ERR_INVALID_URL
} OtaCheckResult_t;

/** Start a manifest fetch. The manifest URL must use HTTPS and be served by
 * the OTA Worker contract; a valid manifest automatically starts the binary
 * download after its integrity fields have been parsed. */
bool OTAService_StartManifestCheck(const char *manifest_url);
bool OTAService_SetPolicy(bool enabled, uint32_t interval_ms, const char *manifest_url);
bool OTAService_RequestCheckNow(void);
OtaCheckResult_t OTAService_RequestCheckNowResult(void);

typedef struct __attribute__((packed)) {
    uint32_t status;
    uint32_t version;
    uint32_t image_size;
    uint32_t downloaded_bytes;
    uint32_t boot_request;
    uint32_t boot_attempts;
    uint32_t policy_enabled;
} OtaStatusView_t;

void OTAService_GetStatus(OtaStatusView_t *out_status);

/**
 * @brief Start OTA firmware download over HTTP using Quectel module.
 * @param url HTTPS URL to the firmware binary
 * @param version Target version code
 * @param expected_size Binary file size in bytes (max 120 KB)
 * @param expected_crc32 Expected CRC32 checksum of the file
 * @param expected_sha256 Expected SHA-256 digest of the file
 */
bool OTAService_StartDownload(const char *url, uint32_t version,
                              uint32_t expected_size, uint32_t expected_crc32,
                              const uint8_t expected_sha256[OTA_SHA256_DIGEST_SIZE]);

/**
 * @brief Abort ongoing OTA download and reset state.
 */
void OTAService_Abort(void);

/**
 * @brief Get current OTA descriptor and progress.
 */
void OTAService_GetDescriptor(OtaDescriptor_t *out_desc);

/**
 * @brief Set boot request flag in SPI Flash to instruct bootloader on next reset.
 */
bool OTAService_RequestApply(void);

/**
 * @brief Run hardware self-test on external SPI Flash.
 */
bool OTAService_SelfTestFlash(uint32_t *out_jedec, uint32_t *out_cap_kb);

/**
 * @brief Direct firmware upload over USB / PC protocol into SPI Flash staging area.
 */
bool OTAService_DirectUploadStart(uint32_t total_size, uint32_t expected_crc32, uint32_t version);
bool OTAService_DirectUploadChunk(uint32_t offset, const uint8_t *data, uint16_t len);
bool OTAService_DirectUploadFinish(void);

#ifdef __cplusplus
}
#endif

#endif /* OTA_SERVICE_H */
