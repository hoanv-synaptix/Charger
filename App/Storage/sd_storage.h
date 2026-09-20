/**
 * @file    sd_storage.h
 * @brief   Minimal FatFs volume lifecycle for the optional SD card.
 */

#ifndef SD_STORAGE_H
#define SD_STORAGE_H

#include "ff.h"
#include "bsp_sd_card.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

bool SDStorage_Mount(void);
void SDStorage_Unmount(void);
bool SDStorage_IsMounted(void);
FRESULT SDStorage_GetLastResult(void);

typedef struct {
    bool card_present;
    bool card_ready;
    bool mounted;
    bool sector0_read;
    bool mbr_signature;
    bool file_write;
    bool file_read;
    bool file_match;
    bool file_removed;
    uint32_t block_count;
    SD_IoStage_t read_stage;
    uint8_t read_response;
    FRESULT last_result;
} SDStorageTestResult_t;

bool SDStorage_RunSelfTest(SDStorageTestResult_t *result);

#ifdef __cplusplus
}
#endif

#endif /* SD_STORAGE_H */
