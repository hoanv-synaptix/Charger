/**
 * @file    bsp_spi_flash.h
 * @brief   Board Support Package - External SPI NOR Flash Driver (SPI2)
 * @note    Compatible with standard JEDEC SPI Flash (Winbond W25Qxx, GigaDevice GD25Qxx)
 */

#ifndef BSP_SPI_FLASH_H
#define BSP_SPI_FLASH_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Standard SPI Flash Specifications */
#define SPI_FLASH_PAGE_SIZE             256U    /* 256 bytes per page */
#define SPI_FLASH_SECTOR_SIZE           4096U   /* 4 KB per sector */
#define SPI_FLASH_BLOCK32_SIZE          32768U  /* 32 KB per block */
#define SPI_FLASH_BLOCK64_SIZE          65536U  /* 64 KB per block */

/* Partition Layout in External Flash */
#define SPI_FLASH_CONFIG_BASE           0x00000000U /* Sector 0 (4 KB) for ChargeCycleConfig */
#define SPI_FLASH_ENERGY_BASE           0x00001000U /* Sectors 1..4 (16 KB) for Energy Journal */
#define SPI_FLASH_ENERGY_SECTOR_COUNT   4U
#define SPI_FLASH_LOG_BASE              0x00005000U /* Sectors 5..12 (32 KB) for Alarm Storage (1024 events) */
#define SPI_FLASH_OTA_META_BASE         0x00010000U /* Sector 16 (4 KB) for OTA Descriptor */
#define SPI_FLASH_OTA_META_MIRROR_BASE  0x00011000U /* Sector 17: power-fail-safe mirror */
#define SPI_FLASH_OTA_STAGING_BASE      0x00020000U /* Sectors 32..63 (128 KB) for OTA Firmware Staging */
#define SPI_FLASH_OTA_STAGING_SIZE      (128U * 1024U)
#define SPI_FLASH_OTA_BACKUP_BASE       0x00040000U /* Sectors 64..95 (128 KB) */
#define SPI_FLASH_OTA_BACKUP_SIZE       (128U * 1024U)

#if (SPI_FLASH_OTA_META_BASE + SPI_FLASH_SECTOR_SIZE) > SPI_FLASH_OTA_STAGING_BASE
#error "OTA metadata overlaps staging area"
#endif
#if (SPI_FLASH_OTA_STAGING_BASE + SPI_FLASH_OTA_STAGING_SIZE) > SPI_FLASH_OTA_BACKUP_BASE
#error "OTA staging overlaps backup area"
#endif

/* Public API */

/**
 * @brief Initialize SPI Flash hardware and verify JEDEC ID
 * @return true if valid SPI Flash detected, false otherwise
 */
bool BSP_SPIFlash_Init(void);

/**
 * @brief Check if SPI Flash is detected and communication is healthy
 */
bool BSP_SPIFlash_IsAvailable(void);

/**
 * @brief Get detected capacity in bytes (e.g. 4194304 for 4MB W25Q32)
 */
uint32_t BSP_SPIFlash_GetCapacity(void);

/**
 * @brief Get JEDEC ID (24 bits: Mfr ID, Memory Type, Capacity)
 */
uint32_t BSP_SPIFlash_GetJedecId(void);

/**
 * @brief Read arbitrary length data from SPI Flash
 * @param address Flash byte address (0..Capacity-1)
 * @param buf Destination buffer
 * @param len Number of bytes to read
 * @return true on success, false on error
 */
bool BSP_SPIFlash_Read(uint32_t address, uint8_t *buf, uint32_t len);

/**
 * @brief Write arbitrary length data to SPI Flash (handles page crossing automatically)
 * @param address Flash byte address (must be erased beforehand if rewriting)
 * @param data Source buffer
 * @param len Number of bytes to write
 * @return true on success, false on error
 */
bool BSP_SPIFlash_Write(uint32_t address, const uint8_t *data, uint32_t len);

/**
 * @brief Erase a 4 KB sector
 * @param address Any address within the 4 KB sector
 * @return true on success, false on error
 */
bool BSP_SPIFlash_EraseSector4K(uint32_t address);

/**
 * @brief Erase a 32 KB block
 * @param address Any address within the 32 KB block
 * @return true on success, false on error
 */
bool BSP_SPIFlash_EraseBlock32K(uint32_t address);

/**
 * @brief Erase a 64 KB block
 * @param address Any address within the 64 KB block
 * @return true on success, false on error
 */
bool BSP_SPIFlash_EraseBlock64K(uint32_t address);

/**
 * @brief Erase entire chip (WARNING: Takes several seconds)
 * @return true on success, false on error
 */
bool BSP_SPIFlash_ChipErase(void);

#ifdef __cplusplus
}
#endif

#endif /* BSP_SPI_FLASH_H */
