/**
 * @file    ota_service.c
 * @brief   Application OTA Layer - Firmware Download & Verification Service
 */

#include "ota_service.h"
#include "main.h"
#include "bsp_spi_flash.h"
#include "bsp_quectel.h"
#include "quectel_at_engine.h"
#include "debug_log.h"
#include <string.h>
#include <stdio.h>

/* IEEE 802.3 CRC32 Implementation */
static uint32_t crc32_update(uint32_t crc, const uint8_t *data, size_t len)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            crc = (crc >> 1) ^ (0xEDB88320U & (-(int32_t)(crc & 1U)));
        }
    }
    return ~crc;
}

static OtaDescriptor_t s_desc;
static char s_url[128] = {0};
static uint32_t s_step_tick = 0U;

typedef enum {
    OTA_STEP_IDLE = 0,
    OTA_STEP_PREPARE_FLASH,
    OTA_STEP_SET_URL_CMD,
    OTA_STEP_WAIT_CONNECT_URL,
    OTA_STEP_SEND_URL_BODY,
    OTA_STEP_SEND_GET,
    OTA_STEP_WAIT_GET_RESP,
    OTA_STEP_SEND_READ,
    OTA_STEP_READING_STREAM,
    OTA_STEP_VERIFY,
    OTA_STEP_ERROR
} OtaStep_t;

static OtaStep_t s_step = OTA_STEP_IDLE;

static bool save_descriptor(void)
{
    s_desc.header_crc32 = crc32_update(0U, (const uint8_t *)&s_desc, sizeof(s_desc) - 4U);
    if (!BSP_SPIFlash_IsAvailable()) {
        return false;
    }
    if (!BSP_SPIFlash_EraseSector4K(SPI_FLASH_OTA_META_BASE)) {
        return false;
    }
    return BSP_SPIFlash_Write(SPI_FLASH_OTA_META_BASE, (const uint8_t *)&s_desc, sizeof(s_desc));
}

static void load_descriptor(void)
{
    if (!BSP_SPIFlash_IsAvailable()) {
        memset(&s_desc, 0, sizeof(s_desc));
        return;
    }
    BSP_SPIFlash_Read(SPI_FLASH_OTA_META_BASE, (uint8_t *)&s_desc, sizeof(s_desc));
    if (s_desc.magic != OTA_MAGIC_HEADER) {
        memset(&s_desc, 0, sizeof(s_desc));
        s_desc.magic = OTA_MAGIC_HEADER;
        s_desc.status = OTA_STATUS_IDLE;
        s_desc.target_mcu = OTA_TARGET_STM32G0B1;
    } else {
        uint32_t chk = crc32_update(0U, (const uint8_t *)&s_desc, sizeof(s_desc) - 4U);
        if (chk != s_desc.header_crc32) {
            LOG("OTA: Header CRC mismatch, resetting descriptor\r\n");
            memset(&s_desc, 0, sizeof(s_desc));
            s_desc.magic = OTA_MAGIC_HEADER;
            s_desc.status = OTA_STATUS_IDLE;
        }
    }
}

void OTAService_Init(void)
{
    load_descriptor();
    s_step = OTA_STEP_IDLE;
    LOG("OTA_Service: Initialized (Status=%lu, BootReq=0x%08lX)\r\n",
        (unsigned long)s_desc.status, (unsigned long)s_desc.boot_request);
}

bool OTAService_StartDownload(const char *url, uint32_t version, uint32_t expected_size, uint32_t expected_crc32)
{
    if (url == NULL || expected_size == 0U || expected_size > SPI_FLASH_OTA_STAGING_SIZE) {
        LOG("OTA_Service: Invalid parameters (size=%lu)\r\n", (unsigned long)expected_size);
        return false;
    }
    if (!QuectelEngine_IsNetReady()) {
        LOG("OTA_Service: Network not ready for download\r\n");
        return false;
    }

    strncpy(s_url, url, sizeof(s_url) - 1U);
    s_url[sizeof(s_url) - 1U] = '\0';

    s_desc.magic = OTA_MAGIC_HEADER;
    s_desc.version = version;
    s_desc.image_size = expected_size;
    s_desc.image_crc32 = expected_crc32;
    s_desc.target_mcu = OTA_TARGET_STM32G0B1;
    s_desc.status = OTA_STATUS_DOWNLOADING;
    s_desc.boot_request = OTA_BOOT_FLAG_CLEARED;
    s_desc.downloaded_bytes = 0U;
    s_desc.calc_crc32 = 0U;
    save_descriptor();

    s_step = OTA_STEP_PREPARE_FLASH;
    s_step_tick = HAL_GetTick();
    LOG("OTA_Service: Starting download of %lu bytes from %s\r\n",
        (unsigned long)expected_size, s_url);
    return true;
}

void OTAService_Abort(void)
{
    s_step = OTA_STEP_IDLE;
    s_desc.status = OTA_STATUS_IDLE;
    save_descriptor();
    LOG("OTA_Service: Download aborted\r\n");
}

void OTAService_GetDescriptor(OtaDescriptor_t *out_desc)
{
    if (out_desc != NULL) {
        memcpy(out_desc, &s_desc, sizeof(OtaDescriptor_t));
    }
}

bool OTAService_RequestApply(void)
{
    if (s_desc.status != OTA_STATUS_VERIFIED) {
        LOG("OTA_Service: Cannot apply, image not verified!\r\n");
        return false;
    }
    s_desc.boot_request = OTA_BOOT_FLAG_REQUEST;
    return save_descriptor();
}

void OTAService_Process(uint32_t now_tick)
{
    char line[128];

    switch (s_step) {
    case OTA_STEP_IDLE:
        break;

    case OTA_STEP_PREPARE_FLASH: {
        /* Erase sectors needed for staging binary */
        uint32_t sectors_needed = (s_desc.image_size + SPI_FLASH_SECTOR_SIZE - 1U) / SPI_FLASH_SECTOR_SIZE;
        LOG("OTA_Service: Erasing %lu sectors for staging image...\r\n", (unsigned long)sectors_needed);
        bool erase_ok = true;
        for (uint32_t i = 0; i < sectors_needed; i++) {
            uint32_t addr = SPI_FLASH_OTA_STAGING_BASE + (i * SPI_FLASH_SECTOR_SIZE);
            if (!BSP_SPIFlash_EraseSector4K(addr)) {
                erase_ok = false;
                break;
            }
        }
        if (!erase_ok) {
            LOG("OTA_Service: Failed to erase flash sectors\r\n");
            s_desc.status = OTA_STATUS_ERROR_FLASH;
            save_descriptor();
            s_step = OTA_STEP_ERROR;
        } else {
            s_step = OTA_STEP_SET_URL_CMD;
            s_step_tick = now_tick;
        }
        break;
    }

    case OTA_STEP_SET_URL_CMD: {
        char cmd[64];
        snprintf(cmd, sizeof(cmd), "AT+QHTTPURL=%u,30", (unsigned)strlen(s_url));
        BSP_Quectel_SendCmd(cmd);
        s_step = OTA_STEP_WAIT_CONNECT_URL;
        s_step_tick = now_tick;
        break;
    }

    case OTA_STEP_WAIT_CONNECT_URL:
        if (BSP_Quectel_ReadLine(line, sizeof(line))) {
            if (strstr(line, "CONNECT") != NULL) {
                /* Send URL body */
                BSP_Quectel_SendString(s_url);
                s_step = OTA_STEP_SEND_URL_BODY;
                s_step_tick = now_tick;
            } else if (strstr(line, "ERROR") != NULL) {
                s_desc.status = OTA_STATUS_ERROR_NETWORK;
                save_descriptor();
                s_step = OTA_STEP_ERROR;
            }
        } else if ((now_tick - s_step_tick) > 5000U) {
            s_step = OTA_STEP_ERROR;
        }
        break;

    case OTA_STEP_SEND_URL_BODY:
        if (BSP_Quectel_ReadLine(line, sizeof(line))) {
            if (strcmp(line, "OK") == 0) {
                /* URL configured, trigger HTTP GET */
                BSP_Quectel_SendCmd("AT+QHTTPGET=60");
                s_step = OTA_STEP_WAIT_GET_RESP;
                s_step_tick = now_tick;
            } else if (strstr(line, "ERROR") != NULL) {
                s_step = OTA_STEP_ERROR;
            }
        } else if ((now_tick - s_step_tick) > 5000U) {
            s_step = OTA_STEP_ERROR;
        }
        break;

    case OTA_STEP_WAIT_GET_RESP:
        if (BSP_Quectel_ReadLine(line, sizeof(line))) {
            if (strncmp(line, "+QHTTPGET:", 10) == 0) {
                int err = 0, status_code = 0, content_len = 0;
                if (sscanf(line + 10, "%d,%d,%d", &err, &status_code, &content_len) >= 2) {
                    if (err == 0 && status_code == 200) {
                        LOG("OTA_Service: HTTP 200 OK (size=%d), reading content...\r\n", content_len);
                        BSP_Quectel_SendCmd("AT+QHTTPREAD=60");
                        s_step = OTA_STEP_READING_STREAM;
                        s_step_tick = now_tick;
                    } else {
                        LOG("OTA_Service: HTTP GET failed (err=%d, status=%d)\r\n", err, status_code);
                        s_desc.status = OTA_STATUS_ERROR_NETWORK;
                        save_descriptor();
                        s_step = OTA_STEP_ERROR;
                    }
                }
            } else if (strstr(line, "ERROR") != NULL) {
                s_step = OTA_STEP_ERROR;
            }
        } else if ((now_tick - s_step_tick) > 60000U) {
            s_step = OTA_STEP_ERROR;
        }
        break;

    case OTA_STEP_READING_STREAM: {
        /* Read binary chunk from UART ring buffer */
        uint8_t chunk[256];
        uint16_t avail = BSP_Quectel_Read(chunk, sizeof(chunk));
        if (avail > 0U) {
            uint32_t addr = SPI_FLASH_OTA_STAGING_BASE + s_desc.downloaded_bytes;
            BSP_SPIFlash_Write(addr, chunk, avail);
            s_desc.calc_crc32 = crc32_update(s_desc.calc_crc32, chunk, avail);
            s_desc.downloaded_bytes += avail;
            s_step_tick = now_tick;

            if (s_desc.downloaded_bytes >= s_desc.image_size) {
                LOG("OTA_Service: Stream download complete (%lu bytes)\r\n",
                    (unsigned long)s_desc.downloaded_bytes);
                s_step = OTA_STEP_VERIFY;
            }
        } else if ((now_tick - s_step_tick) > 10000U) {
            LOG("OTA_Service: Stream read timeout\r\n");
            s_step = OTA_STEP_ERROR;
        }
        break;
    }

    case OTA_STEP_VERIFY:
        LOG("OTA_Service: Verifying CRC32 (calc=0x%08lX, exp=0x%08lX)...\r\n",
            (unsigned long)s_desc.calc_crc32, (unsigned long)s_desc.image_crc32);
        if (s_desc.downloaded_bytes != s_desc.image_size) {
            s_desc.status = OTA_STATUS_ERROR_SIZE;
        } else if (s_desc.calc_crc32 != s_desc.image_crc32) {
            s_desc.status = OTA_STATUS_ERROR_CRC;
        } else {
            s_desc.status = OTA_STATUS_VERIFIED;
            LOG("OTA_Service: Firmware image VERIFIED successfully!\r\n");
        }
        save_descriptor();
        s_step = OTA_STEP_IDLE;
        break;

    case OTA_STEP_ERROR:
        LOG("OTA_Service: Error encountered during OTA operation\r\n");
        s_step = OTA_STEP_IDLE;
        break;

    default:
        s_step = OTA_STEP_IDLE;
        break;
    }
}
