/**
 * @file    sd_storage.c
 * @brief   Minimal FatFs volume lifecycle for the optional SD card.
 */

#include "sd_storage.h"

#include "bsp_sd_card.h"

#include <stddef.h>
#include <string.h>

static FATFS s_fatfs;
static bool s_mounted;
static FRESULT s_last_result = FR_NOT_READY;

bool SDStorage_Mount(void)
{
    if (!BSP_SDCard_IsReady() && !BSP_SDCard_Init()) {
        s_mounted = false;
        s_last_result = FR_NOT_READY;
        return false;
    }

    s_last_result = f_mount(&s_fatfs, "", 1U);
    s_mounted = s_last_result == FR_OK;
    return s_mounted;
}

void SDStorage_Unmount(void)
{
    s_last_result = f_mount(NULL, "", 0U);
    s_mounted = false;
}

bool SDStorage_IsMounted(void)
{
    return s_mounted;
}

FRESULT SDStorage_GetLastResult(void)
{
    return s_last_result;
}

bool SDStorage_RunSelfTest(SDStorageTestResult_t *result)
{
    static const BYTE test_data[] = "CHARGER_SD_RW_TEST_20260919";
    BYTE sector[SD_BLOCK_SIZE];
    BYTE readback[sizeof(test_data) - 1U];
    FIL file;
    UINT transferred;
    FRESULT file_result;
    bool ok;

    if (result == NULL) {
        return false;
    }
    memset(result, 0, sizeof(*result));
    result->card_present = BSP_SDCard_IsPresent();
    result->card_ready = BSP_SDCard_IsReady();
    result->block_count = BSP_SDCard_GetBlockCount();
    result->last_result = FR_NOT_READY;

    if (!result->card_ready && !BSP_SDCard_Init()) {
        return false;
    }
    result->card_ready = BSP_SDCard_IsReady();
    result->block_count = BSP_SDCard_GetBlockCount();
    if (!result->card_ready) {
        return false;
    }

    result->sector0_read = BSP_SDCard_ReadBlock(0U, sector);
    result->read_stage = BSP_SDCard_GetLastReadStage();
    result->read_response = BSP_SDCard_GetLastReadResponse();
    result->mbr_signature = result->sector0_read && sector[510] == 0x55U &&
                            sector[511] == 0xAAU;

    if (!s_mounted && !SDStorage_Mount()) {
        result->last_result = s_last_result;
        return false;
    }
    result->mounted = s_mounted;
    result->last_result = s_last_result;

    file_result = f_open(&file, "SDTEST.TXT", FA_CREATE_ALWAYS | FA_WRITE);
    if (file_result == FR_OK) {
        transferred = 0U;
        file_result = f_write(&file, test_data, sizeof(test_data) - 1U,
                              &transferred);
        result->file_write = file_result == FR_OK &&
                             transferred == sizeof(test_data) - 1U;
        if (result->file_write) {
            if (f_sync(&file) != FR_OK) {
                result->file_write = false;
            }
        }
        if (f_close(&file) != FR_OK) {
            result->file_write = false;
        }
    }
    if (file_result != FR_OK) {
        result->last_result = file_result;
        return false;
    }

    file_result = f_open(&file, "SDTEST.TXT", FA_READ);
    if (file_result == FR_OK) {
        transferred = 0U;
        file_result = f_read(&file, readback, sizeof(readback), &transferred);
        result->file_read = file_result == FR_OK &&
                            transferred == sizeof(readback);
        result->file_match = result->file_read &&
                             memcmp(readback, test_data, sizeof(readback)) == 0;
        if (f_close(&file) != FR_OK) {
            result->file_read = false;
            result->file_match = false;
        }
    }
    if (file_result != FR_OK) {
        result->last_result = file_result;
    }

    result->file_removed = f_unlink("SDTEST.TXT") == FR_OK;
    ok = result->card_ready && result->mounted && result->sector0_read &&
         result->mbr_signature && result->file_write && result->file_read &&
         result->file_match && result->file_removed;
    return ok;
}
