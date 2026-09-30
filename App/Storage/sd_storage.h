/**
 * @file    sd_storage.h
 * @brief   Minimal FatFs volume lifecycle for the optional SD card.
 */

#ifndef SD_STORAGE_H
#define SD_STORAGE_H

#if defined(CHG_ENABLE_SD_CARD) && (CHG_ENABLE_SD_CARD != 0)
#include "ff.h"
#else
typedef enum {
    FR_OK = 0,
    FR_DISK_ERR,
    FR_INT_ERR,
    FR_NOT_READY,
    FR_NO_FILE,
    FR_NO_PATH,
    FR_INVALID_NAME,
    FR_DENIED,
    FR_EXIST,
    FR_INVALID_OBJECT,
    FR_WRITE_PROTECTED,
    FR_INVALID_DRIVE,
    FR_NOT_ENABLED,
    FR_NO_FILESYSTEM,
    FR_MKFS_ABORTED,
    FR_TIMEOUT,
    FR_LOCKED,
    FR_NOT_ENOUGH_CORE,
    FR_TOO_MANY_OPEN_FILES,
    FR_INVALID_PARAMETER
} FRESULT;
#endif

#include "bsp_sd_card.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

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

#if defined(CHG_ENABLE_SD_CARD) && (CHG_ENABLE_SD_CARD != 0)
bool SDStorage_Mount(void);
void SDStorage_Unmount(void);
bool SDStorage_IsMounted(void);
FRESULT SDStorage_GetLastResult(void);
bool SDStorage_RunSelfTest(SDStorageTestResult_t *result);
#else
static inline bool SDStorage_Mount(void) { return false; }
static inline void SDStorage_Unmount(void) {}
static inline bool SDStorage_IsMounted(void) { return false; }
static inline FRESULT SDStorage_GetLastResult(void) { return FR_NOT_READY; }
static inline bool SDStorage_RunSelfTest(SDStorageTestResult_t *result) {
    if (result != NULL) {
        memset(result, 0, sizeof(*result));
        result->last_result = FR_NOT_READY;
    }
    return false;
}
#endif

#ifdef __cplusplus
}
#endif

#endif /* SD_STORAGE_H */
