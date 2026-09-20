/**
 * @file    bsp_sd_card.h
 * @brief   Board Support Package - SPI-mode SD/SDHC Card Driver (SPI1)
 * @note    Implements SPI-mode SD protocol on hspi1 (PA5/PA6/PA7) with PB0 CS.
 *          Supports SDSC (v1) and SDHC/SDXC (v2) cards.
 */

#ifndef BSP_SD_CARD_H
#define BSP_SD_CARD_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* SD Card Block Size (fixed 512 bytes in SPI mode) */
#define SD_BLOCK_SIZE               512U

/* SD Card Types */
typedef enum {
    SD_TYPE_NONE  = 0,  /**< No card or init failed */
    SD_TYPE_SDSC  = 1,  /**< Standard Capacity (≤2 GB), byte addressing */
    SD_TYPE_SDHC  = 2,  /**< High / Extended Capacity (>2 GB), block addressing */
} SD_CardType_t;

typedef enum {
    SD_IO_STAGE_NONE = 0,
    SD_IO_STAGE_NOT_READY,
    SD_IO_STAGE_COMMAND,
    SD_IO_STAGE_TOKEN,
    SD_IO_STAGE_DATA,
    SD_IO_STAGE_CRC
} SD_IoStage_t;

/**
 * @brief  Initialize SD card over SPI1.
 *         Runs at 250 kHz during handshake, then switches to 16 MHz.
 *         May be called once after power-on, or after card hot-insert.
 * @return true if a card was detected and initialized successfully
 */
bool BSP_SDCard_Init(void);

/**
 * @brief  Check physical card presence via card-detect GPIO (PB2, active-low).
 * @return true if card is mechanically inserted
 */
bool BSP_SDCard_IsPresent(void);

/**
 * @brief  Check if the card has been successfully initialized and is ready for I/O.
 * @return true if ready
 */
bool BSP_SDCard_IsReady(void);

/**
 * @brief  Get detected card type.
 * @return SD_CardType_t value
 */
SD_CardType_t BSP_SDCard_GetType(void);

/**
 * @brief  Get total number of 512-byte blocks on the card.
 * @return Block count, or 0 if card not ready
 */
uint32_t BSP_SDCard_GetBlockCount(void);

SD_IoStage_t BSP_SDCard_GetLastReadStage(void);
uint8_t BSP_SDCard_GetLastReadResponse(void);

/**
 * @brief  Read a single 512-byte block.
 * @param  block  Logical block address (0-based)
 * @param  buf    Output buffer, must be at least 512 bytes
 * @return true on success
 */
bool BSP_SDCard_ReadBlock(uint32_t block, uint8_t *buf);

/**
 * @brief  Write a single 512-byte block.
 * @param  block  Logical block address (0-based)
 * @param  buf    Source buffer, exactly 512 bytes
 * @return true on success
 */
bool BSP_SDCard_WriteBlock(uint32_t block, const uint8_t *buf);

/**
 * @brief  Read multiple consecutive 512-byte blocks.
 * @param  block  Starting logical block address
 * @param  buf    Output buffer, must be at least count * 512 bytes
 * @param  count  Number of blocks to read
 * @return true on success
 */
bool BSP_SDCard_ReadBlocks(uint32_t block, uint8_t *buf, uint32_t count);

/**
 * @brief  Write multiple consecutive 512-byte blocks.
 * @param  block  Starting logical block address
 * @param  buf    Source buffer, must be count * 512 bytes
 * @param  count  Number of blocks to write
 * @return true on success
 */
bool BSP_SDCard_WriteBlocks(uint32_t block, const uint8_t *buf, uint32_t count);

/**
 * @brief  Self-test: init, read block 0 (MBR), verify boot signature 0x55AA.
 * @return true if all steps pass
 */
bool BSP_SDCard_SelfTest(void);

#ifdef __cplusplus
}
#endif

#endif /* BSP_SD_CARD_H */
