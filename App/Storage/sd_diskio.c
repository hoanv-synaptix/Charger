/**
 * @file    sd_diskio.c
 * @brief   FatFs disk-I/O adapter for the SPI SD card BSP.
 */

#include "sd_diskio.h"

#include "bsp_rtc.h"
#include "bsp_sd_card.h"

#define SD_DISK_NUMBER       0U
#define SD_SECTOR_SIZE       512U

static DSTATUS sd_status(void)
{
    if (!BSP_SDCard_IsPresent()) {
        return STA_NOINIT | STA_NODISK;
    }
    return BSP_SDCard_IsReady() ? 0U : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != SD_DISK_NUMBER) {
        return STA_NOINIT;
    }
    if (!BSP_SDCard_IsPresent()) {
        return STA_NOINIT | STA_NODISK;
    }
    return BSP_SDCard_Init() ? 0U : STA_NOINIT;
}

DSTATUS disk_status(BYTE pdrv)
{
    return pdrv == SD_DISK_NUMBER ? sd_status() : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    uint32_t block;

    if (pdrv != SD_DISK_NUMBER || buff == NULL || count == 0U ||
        sector > UINT32_MAX) {
        return RES_PARERR;
    }

    block = (uint32_t)sector;
    if (!BSP_SDCard_IsReady() ||
        block >= BSP_SDCard_GetBlockCount() ||
        (uint32_t)count > BSP_SDCard_GetBlockCount() - block) {
        return RES_NOTRDY;
    }

    return BSP_SDCard_ReadBlocks(block, buff, (uint32_t)count)
               ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    uint32_t block;

    if (pdrv != SD_DISK_NUMBER || buff == NULL || count == 0U ||
        sector > UINT32_MAX) {
        return RES_PARERR;
    }

    block = (uint32_t)sector;
    if (!BSP_SDCard_IsReady() ||
        block >= BSP_SDCard_GetBlockCount() ||
        (uint32_t)count > BSP_SDCard_GetBlockCount() - block) {
        return RES_NOTRDY;
    }

    return BSP_SDCard_WriteBlocks(block, buff, (uint32_t)count)
               ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != SD_DISK_NUMBER) {
        return RES_PARERR;
    }
    if (!BSP_SDCard_IsReady()) {
        return RES_NOTRDY;
    }

    switch (cmd) {
        case CTRL_SYNC:
            return RES_OK;

        case GET_SECTOR_COUNT:
            if (buff == NULL) return RES_PARERR;
            *(DWORD *)buff = (DWORD)BSP_SDCard_GetBlockCount();
            return RES_OK;

        case GET_SECTOR_SIZE:
            if (buff == NULL) return RES_PARERR;
            *(WORD *)buff = SD_SECTOR_SIZE;
            return RES_OK;

        case GET_BLOCK_SIZE:
            /* The raw SPI driver has no erase-group query. One sector is a
             * safe lower bound for FatFs formatting decisions. */
            if (buff == NULL) return RES_PARERR;
            *(DWORD *)buff = 1U;
            return RES_OK;

        default:
            return RES_PARERR;
    }
}

DWORD get_fattime(void)
{
    BSP_RTC_DateTime_t date_time;

    if (!BSP_RTC_GetDateTime(&date_time) || date_time.year < 1980U ||
        date_time.month == 0U || date_time.month > 12U ||
        date_time.day == 0U || date_time.day > 31U ||
        date_time.hour > 23U || date_time.minute > 59U ||
        date_time.second > 59U) {
        return ((DWORD)(2024U - 1980U) << 25U) |
               ((DWORD)1U << 21U) | ((DWORD)1U << 16U);
    }

    return ((DWORD)(date_time.year - 1980U) << 25U) |
           ((DWORD)date_time.month << 21U) |
           ((DWORD)date_time.day << 16U) |
           ((DWORD)date_time.hour << 11U) |
           ((DWORD)date_time.minute << 5U) |
           ((DWORD)date_time.second >> 1U);
}
