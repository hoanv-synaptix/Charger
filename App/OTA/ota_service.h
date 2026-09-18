/**
 * @file    ota_service.h
 * @brief   Application OTA Layer - Firmware Download & Verification Service
 */

#ifndef OTA_SERVICE_H
#define OTA_SERVICE_H

#include "ota_types.h"

#ifdef __cplusplus
extern "C" {
#endif

void OTAService_Init(void);
void OTAService_Process(uint32_t now_tick);

/**
 * @brief Start OTA firmware download over HTTP using Quectel module.
 * @param url HTTP URL to the firmware binary
 * @param version Target version code
 * @param expected_size Binary file size in bytes (max 128 KB)
 * @param expected_crc32 Expected CRC32 checksum of the file
 */
bool OTAService_StartDownload(const char *url, uint32_t version, uint32_t expected_size, uint32_t expected_crc32);

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

#ifdef __cplusplus
}
#endif

#endif /* OTA_SERVICE_H */
