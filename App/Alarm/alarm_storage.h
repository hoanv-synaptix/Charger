/**
 * @file    alarm_storage.h
 * @brief   Persistent Alarm Event Log storage in External SPI Flash
 * @note    Uses a circular 8-sector (32 KB) journal, storing up to 1024 events.
 */

#ifndef APP_ALARM_ALARM_STORAGE_H
#define APP_ALARM_ALARM_STORAGE_H

#include <stdint.h>
#include <stdbool.h>
#include "alarm.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ALARM_RECORD_MAGIC          0x414C4D31U /* "ALM1" */
#define ALARM_RECORD_VERSION        1U
#define ALARM_RECORD_LENGTH         32U

/* Flash Layout: Sectors 5..12 (8 sectors x 4KB = 32 KB) */
#define SPI_FLASH_ALARM_BASE        0x00005000U
#define SPI_FLASH_ALARM_SECTOR_SIZE 4096U
#define SPI_FLASH_ALARM_SECTORS     8U
#define SPI_FLASH_ALARM_MAX_RECORDS (SPI_FLASH_ALARM_SECTORS * (SPI_FLASH_ALARM_SECTOR_SIZE / ALARM_RECORD_LENGTH)) /* 1024 records */

typedef struct __attribute__((packed)) {
    uint32_t magic;         /* 0x414C4D31 */
    uint32_t timestamp_s;   /* Epoch timestamp from RTC (or 0 if not set) */
    uint32_t uptime_ms;     /* System tick at event */
    uint16_t code;          /* AlarmCode_t */
    uint8_t  action;        /* AlarmAction_t */
    uint8_t  event;         /* 1 = RAISED, 0 = CLEARED */
    uint32_t sequence;      /* Monotonically increasing sequence number */
    uint16_t crc16;         /* CRC-16 checksum */
    uint8_t  reserved[10];  /* Padding to exactly 32 bytes */
} AlarmPersistentRecord_t;

_Static_assert(sizeof(AlarmPersistentRecord_t) == ALARM_RECORD_LENGTH,
               "AlarmPersistentRecord_t must be exactly 32 bytes");

/**
 * @brief Initialize persistent alarm storage.
 *        Scans SPI Flash journal to find next sequence and restore recent history into RAM.
 * @param ram_log Pointer to RAM log array to populate (up to max_entries)
 * @param max_entries Maximum entries RAM log can hold (e.g. ALARM_LOG_DEPTH = 24)
 * @param out_sequence Pointer to receive the highest sequence count
 * @return Number of entries restored into RAM log
 */
uint8_t AlarmStorage_Init(AlarmLogEntry_t *ram_log, uint8_t max_entries, uint32_t *out_sequence);

/**
 * @brief Append a new alarm event record into External SPI Flash journal.
 * @param now_tick Current uptime tick
 * @param code AlarmCode_t
 * @param action AlarmAction_t
 * @param raised true = RAISED, false = CLEARED
 * @return true on success
 */
bool AlarmStorage_Append(uint32_t now_tick, uint16_t code, uint8_t action, bool raised);

/**
 * @brief Read records from SPI Flash (newest-first)
 * @param out_records Destination buffer
 * @param max_count Maximum records to read
 * @return Number of records read
 */
uint16_t AlarmStorage_ReadRecent(AlarmPersistentRecord_t *out_records, uint16_t max_count);

/**
 * @brief Clear all alarm history from SPI Flash
 */
bool AlarmStorage_ClearAll(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_ALARM_ALARM_STORAGE_H */
