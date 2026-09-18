/**
 * @file    alarm_storage.c
 * @brief   Persistent Alarm Event Log storage in External SPI Flash
 */

#include "alarm_storage.h"
#include "bsp_spi_flash.h"
#include "bsp_rtc.h"
#include "debug_log.h"
#include <string.h>

#define SLOTS_PER_SECTOR  (SPI_FLASH_ALARM_SECTOR_SIZE / ALARM_RECORD_LENGTH) /* 128 */
#define FLASH_BLANK_BYTE  0xFFU

static uint32_t s_next_sequence = 1U;
static uint32_t s_active_sector = 0U;
static uint32_t s_active_slot = 0U;
static bool s_storage_ready = false;

/* CRC-16 CCITT (polynomial 0x1021, init 0xFFFF) */
static uint16_t calc_crc16(const uint8_t *data, uint32_t len)
{
    uint16_t crc = 0xFFFFU;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= ((uint16_t)data[i] << 8);
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x8000U) {
                crc = (crc << 1) ^ 0x1021U;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

static uint32_t record_address(uint32_t sector, uint32_t slot)
{
    return SPI_FLASH_ALARM_BASE + sector * SPI_FLASH_ALARM_SECTOR_SIZE + slot * ALARM_RECORD_LENGTH;
}

static bool is_record_valid(const AlarmPersistentRecord_t *rec)
{
    if (rec == NULL || rec->magic != ALARM_RECORD_MAGIC) {
        return false;
    }
    uint16_t expected_crc = calc_crc16((const uint8_t *)rec, offsetof(AlarmPersistentRecord_t, crc16));
    return rec->crc16 == expected_crc;
}

static bool is_slot_blank(uint32_t addr)
{
    uint8_t buf[ALARM_RECORD_LENGTH];
    if (!BSP_SPIFlash_Read(addr, buf, ALARM_RECORD_LENGTH)) {
        return false;
    }
    for (uint32_t i = 0; i < ALARM_RECORD_LENGTH; i++) {
        if (buf[i] != FLASH_BLANK_BYTE) return false;
    }
    return true;
}

/* ================= Public API ================= */

uint8_t AlarmStorage_Init(AlarmLogEntry_t *ram_log, uint8_t max_entries, uint32_t *out_sequence)
{
    s_storage_ready = false;
    s_next_sequence = 1U;
    s_active_sector = 0U;
    s_active_slot = 0U;

    if (!BSP_SPIFlash_IsAvailable()) {
        LOG("AlarmStorage: SPI Flash not available, persistent alarm log disabled\r\n");
        return 0U;
    }

    uint32_t max_seq = 0U;
    bool found_any = false;
    uint32_t last_valid_sector = 0U;
    uint32_t last_valid_slot = 0U;

    /* Scan all slots to find highest sequence and current write pointer */
    for (uint32_t sec = 0; sec < SPI_FLASH_ALARM_SECTORS; sec++) {
        for (uint32_t slot = 0; slot < SLOTS_PER_SECTOR; slot++) {
            uint32_t addr = record_address(sec, slot);
            AlarmPersistentRecord_t rec;
            if (BSP_SPIFlash_Read(addr, (uint8_t *)&rec, sizeof(rec))) {
                if (is_record_valid(&rec)) {
                    if (!found_any || (int32_t)(rec.sequence - max_seq) > 0) {
                        max_seq = rec.sequence;
                        last_valid_sector = sec;
                        last_valid_slot = slot;
                        found_any = true;
                    }
                }
            }
        }
    }

    if (found_any) {
        s_next_sequence = max_seq + 1U;
        /* Position write pointer right after the highest valid record */
        if (last_valid_slot + 1U < SLOTS_PER_SECTOR) {
            s_active_sector = last_valid_sector;
            s_active_slot = last_valid_slot + 1U;
        } else {
            s_active_sector = (last_valid_sector + 1U) % SPI_FLASH_ALARM_SECTORS;
            s_active_slot = 0U;
        }
        LOG("AlarmStorage: Found persistent log (seq=%lu, active_sec=%lu, active_slot=%lu)\r\n",
            (unsigned long)max_seq, (unsigned long)s_active_sector, (unsigned long)s_active_slot);
    } else {
        LOG("AlarmStorage: Initialized fresh alarm journal at Sector 5 (32 KB)\r\n");
        s_active_sector = 0U;
        s_active_slot = 0U;
        BSP_SPIFlash_EraseSector4K(record_address(0U, 0U));
    }

    s_storage_ready = true;
    if (out_sequence != NULL) {
        *out_sequence = max_seq;
    }

    /* Restore recent history into RAM log (up to max_entries, newest first) */
    uint8_t restored = 0U;
    if (found_any && ram_log != NULL && max_entries > 0U) {
        AlarmPersistentRecord_t recs[ALARM_LOG_DEPTH];
        uint16_t count = AlarmStorage_ReadRecent(recs, (max_entries < ALARM_LOG_DEPTH) ? max_entries : ALARM_LOG_DEPTH);
        for (uint16_t i = 0; i < count; i++) {
            ram_log[i].uptime_ms = recs[i].uptime_ms;
            ram_log[i].code = recs[i].code;
            ram_log[i].action = recs[i].action;
            ram_log[i].event = recs[i].event;
            restored++;
        }
        LOG("AlarmStorage: Restored %u recent alarm events into RAM log\r\n", (unsigned)restored);
    }

    return restored;
}

bool AlarmStorage_Append(uint32_t now_tick, uint16_t code, uint8_t action, bool raised)
{
    if (!s_storage_ready || !BSP_SPIFlash_IsAvailable()) {
        return false;
    }

    AlarmPersistentRecord_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.magic = ALARM_RECORD_MAGIC;
    rec.timestamp_s = BSP_RTC_IsTimeValid() ? BSP_RTC_GetEpoch() : 0U;
    rec.uptime_ms = now_tick;
    rec.code = code;
    rec.action = action;
    rec.event = raised ? 1U : 0U;
    rec.sequence = s_next_sequence++;
    rec.crc16 = calc_crc16((const uint8_t *)&rec, offsetof(AlarmPersistentRecord_t, crc16));

    uint32_t target_addr = record_address(s_active_sector, s_active_slot);

    /* Check if target slot is blank; if not or if starting a new sector, erase sector */
    if (s_active_slot == 0U && !is_slot_blank(target_addr)) {
        BSP_SPIFlash_EraseSector4K(target_addr);
    } else if (!is_slot_blank(target_addr)) {
        /* Slot is unexpectedly not blank, rotate to next sector */
        s_active_sector = (s_active_sector + 1U) % SPI_FLASH_ALARM_SECTORS;
        s_active_slot = 0U;
        target_addr = record_address(s_active_sector, 0U);
        BSP_SPIFlash_EraseSector4K(target_addr);
    }

    bool ok = BSP_SPIFlash_Write(target_addr, (const uint8_t *)&rec, sizeof(rec));
    if (ok) {
        /* Advance slot */
        s_active_slot++;
        if (s_active_slot >= SLOTS_PER_SECTOR) {
            s_active_slot = 0U;
            s_active_sector = (s_active_sector + 1U) % SPI_FLASH_ALARM_SECTORS;
        }
    } else {
        LOG("AlarmStorage: Write failed at 0x%08lX\r\n", (unsigned long)target_addr);
    }

    return ok;
}

uint16_t AlarmStorage_ReadRecent(AlarmPersistentRecord_t *out_records, uint16_t max_count)
{
    if (!s_storage_ready || out_records == NULL || max_count == 0U) {
        return 0U;
    }

    /* Temporary buffer to collect valid records */
    uint16_t collected = 0U;

    for (uint32_t sec = 0; sec < SPI_FLASH_ALARM_SECTORS && collected < max_count; sec++) {
        for (uint32_t slot = 0; slot < SLOTS_PER_SECTOR && collected < max_count; slot++) {
            uint32_t addr = record_address(sec, slot);
            AlarmPersistentRecord_t rec;
            if (BSP_SPIFlash_Read(addr, (uint8_t *)&rec, sizeof(rec))) {
                if (is_record_valid(&rec)) {
                    out_records[collected++] = rec;
                }
            }
        }
    }

    /* Sort newest-first (descending by sequence) using simple insertion sort */
    for (uint16_t i = 1; i < collected; i++) {
        AlarmPersistentRecord_t key = out_records[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && (int32_t)(key.sequence - out_records[j].sequence) > 0) {
            out_records[j + 1] = out_records[j];
            j--;
        }
        out_records[j + 1] = key;
    }

    return collected;
}

bool AlarmStorage_ClearAll(void)
{
    if (!BSP_SPIFlash_IsAvailable()) return false;
    for (uint32_t sec = 0; sec < SPI_FLASH_ALARM_SECTORS; sec++) {
        BSP_SPIFlash_EraseSector4K(record_address(sec, 0U));
    }
    s_active_sector = 0U;
    s_active_slot = 0U;
    s_next_sequence = 1U;
    LOG("AlarmStorage: All persistent alarm logs cleared\r\n");
    return true;
}
