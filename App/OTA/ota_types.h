/**
 * @file    ota_types.h
 * @brief   OTA Firmware Structures and Constants
 */

#ifndef OTA_TYPES_H
#define OTA_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_MAGIC_HEADER            0x4F544131U  /* "OTA1" */
#define OTA_DESCRIPTOR_VERSION     2U
#define OTA_TARGET_STM32G0B1        0x47304231U  /* "G0B1" */
#define OTA_BOOT_FLAG_REQUEST       0x55AA1234U  /* Triggers bootloader flash copy */
#define OTA_BOOT_FLAG_CLEARED       0x00000000U
#define OTA_HEALTH_MARKER_PENDING   0xFFFFFFFFU
#define OTA_HEALTH_MARKER_CONFIRMED 0xA55AC33CU
#define OTA_MAX_IMAGE_SIZE          (120U * 1024U)

typedef enum {
    OTA_STATUS_IDLE = 0,
    OTA_STATUS_DOWNLOADING,
    OTA_STATUS_DOWNLOADED,
    OTA_STATUS_VERIFIED,
    OTA_STATUS_APPLIED,
    OTA_STATUS_ERROR_SIZE,
    OTA_STATUS_ERROR_CRC,
    OTA_STATUS_ERROR_FLASH,
    OTA_STATUS_ERROR_NETWORK,
    OTA_STATUS_ERROR_TIMEOUT,
    OTA_STATUS_ERROR_ROLLBACK,
    OTA_STATUS_BOOT_TEST
} OtaStatusCode_t;

typedef struct __attribute__((packed)) {
    uint32_t magic;             /* OTA_MAGIC_HEADER */
    uint32_t descriptor_version;
    uint32_t version;           /* E.g. 0x010200 for v1.2.0 */
    uint32_t image_size;        /* Binary size in bytes */
    uint32_t image_crc32;       /* Expected CRC32 of binary */
    uint32_t target_mcu;        /* OTA_TARGET_STM32G0B1 */
    uint32_t status;            /* OtaStatusCode_t */
    uint32_t boot_request;      /* OTA_BOOT_FLAG_REQUEST when ready */
    uint32_t downloaded_bytes;  /* Progress tracker */
    uint32_t calc_crc32;        /* Progress CRC32 */
    uint8_t expected_sha256[32];
    uint8_t calc_sha256[32];
    uint32_t transaction_id;
    uint32_t boot_attempts;
    uint32_t health_marker;
    uint32_t backup_valid;
    uint32_t backup_image_size;
    uint32_t backup_crc32;
    uint32_t policy_enabled;
    uint32_t policy_interval_ms;
    char policy_manifest_url[128];
    uint32_t reserved[1];
    uint32_t header_crc32;      /* CRC32 of the preceding fields */
} OtaDescriptor_t;

#ifdef __cplusplus
}
#endif

#endif /* OTA_TYPES_H */
