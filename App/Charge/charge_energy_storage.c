/**
 * @file charge_energy_storage.c
 * @brief Four-sector journal for accumulated charge and energy counters
 *        using External SPI Flash with automatic Migration from Internal Flash.
 *        Version 2 adds persistent total charge duration (seconds).
 */

#include "charge_energy_storage.h"
#include "bsp_spi_flash.h"
#include "bsp_flash.h"
#include "debug_log.h"

#include <math.h>
#include <string.h>

/* Internal MCU Flash Layout (for legacy reading & migration) */
#define INTERNAL_ENERGY_FLASH_BASE       0x0801D800U /* pages 59..62 */
#define INTERNAL_ENERGY_FLASH_PAGE_SIZE  BSP_FLASH_PAGE_SIZE
#define INTERNAL_ENERGY_FLASH_PAGE_COUNT 4U

/* External SPI Flash Layout */
#define SPI_ENERGY_FLASH_BASE            SPI_FLASH_ENERGY_BASE /* 0x00001000U */
#define SPI_ENERGY_FLASH_SECTOR_SIZE     SPI_FLASH_SECTOR_SIZE /* 4096U */
#define SPI_ENERGY_FLASH_SECTOR_COUNT    SPI_FLASH_ENERGY_SECTOR_COUNT /* 4U */

#define ENERGY_RECORD_MAGIC        0x454E4731U /* "ENG1" */
#define ENERGY_RECORD_VERSION_V1   1U
#define ENERGY_RECORD_LENGTH_V1    32U
#define ENERGY_RECORD_VERSION_V2   2U
#define ENERGY_RECORD_LENGTH_V2    40U

#define ENERGY_RECORD_VERSION      ENERGY_RECORD_VERSION_V2
#define ENERGY_RECORD_LENGTH       ENERGY_RECORD_LENGTH_V2

#define ENERGY_CHECKPOINT_MS       300000U
#define ENERGY_SCALE               1000.0f
#define FLASH_BLANK_BYTE           0xFFU

#ifdef CHARGE_ENERGY_STORAGE_HOST_TEST
extern uint8_t g_energy_test_flash[];
#endif

/* Legacy Version 1 record (32 bytes) */
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t length;
    uint32_t sequence;
    uint64_t total_charged_ah_x1000;
    uint64_t total_energy_kwh_x1000;
    uint32_t crc32;
} EnergyRecordV1_t;

/* Active Version 2 record (40 bytes, 8-byte aligned) */
typedef struct __attribute__((packed)) {
    uint32_t magic;                     /* 0x454E4731 "ENG1" */
    uint16_t version;                   /* 2 */
    uint16_t length;                    /* 40 */
    uint32_t sequence;                  /* Monotonic sequence counter */
    uint64_t total_charged_ah_x1000;    /* Accumulated Ah * 1000 */
    uint64_t total_energy_kwh_x1000;    /* Accumulated kWh * 1000 */
    uint32_t total_charge_seconds;      /* Accumulated charging time in seconds */
    uint32_t reserved;                  /* 0xFFFFFFFF padding */
    uint32_t crc32;                     /* CRC32 from sequence to reserved (28 bytes) */
} EnergyRecord_t;

_Static_assert(sizeof(EnergyRecordV1_t) == ENERGY_RECORD_LENGTH_V1,
               "EnergyRecordV1_t must stay 32 bytes");
_Static_assert(sizeof(EnergyRecord_t) == ENERGY_RECORD_LENGTH_V2,
               "EnergyRecord_t must stay 40 bytes");

static float g_total_charged_ah;
static float g_total_energy_kwh;
static uint32_t g_total_charge_seconds;
static uint64_t g_last_saved_charged_x1000;
static uint64_t g_last_saved_energy_x1000;
static uint32_t g_last_saved_seconds;
static bool g_have_saved_record;
static uint32_t g_next_sequence;
static bool g_initialized;
static bool g_was_charging;
static bool g_reset_request;
static uint32_t g_last_checkpoint_tick;

#ifdef CHARGE_ENERGY_STORAGE_HOST_TEST
void ChargeEnergyStorage_TestResetInternalState(void)
{
    g_initialized = false;
    g_have_saved_record = false;
    g_next_sequence = 0U;
    g_total_charged_ah = 0.0f;
    g_total_energy_kwh = 0.0f;
    g_total_charge_seconds = 0U;
    g_last_saved_charged_x1000 = 0U;
    g_last_saved_energy_x1000 = 0U;
    g_last_saved_seconds = 0U;
}
#endif

static uint32_t calc_crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (uint32_t i = 0U; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0U; bit < 8U; bit++) {
            crc = (crc & 1U) ? ((crc >> 1U) ^ 0xEDB88320U) : (crc >> 1U);
        }
    }
    return ~crc;
}

static bool record_valid(const EnergyRecord_t *record)
{
    if (record == NULL || record->magic != ENERGY_RECORD_MAGIC ||
        record->version != ENERGY_RECORD_VERSION_V2 ||
        record->length != ENERGY_RECORD_LENGTH_V2) {
        return false;
    }
    /* CRC covers sequence, ah, kwh, total_charge_seconds, reserved (28 bytes) */
    return record->crc32 == calc_crc32((const uint8_t *)&record->sequence, 28U);
}

static bool record_v1_valid(const EnergyRecordV1_t *rec_v1)
{
    if (rec_v1 == NULL || rec_v1->magic != ENERGY_RECORD_MAGIC ||
        rec_v1->version != ENERGY_RECORD_VERSION_V1 ||
        rec_v1->length != ENERGY_RECORD_LENGTH_V1) {
        return false;
    }
    return rec_v1->crc32 == calc_crc32((const uint8_t *)&rec_v1->sequence, 20U);
}

static bool sequence_newer(uint32_t candidate, uint32_t reference)
{
    return (int32_t)(candidate - reference) > 0;
}

static bool encode_counter(float value, uint64_t *encoded)
{
    if (encoded == NULL || !isfinite(value) || value < 0.0f ||
        value > (4000000.0f)) {
        return false;
    }
    uint32_t scaled = (uint32_t)(value * ENERGY_SCALE + 0.5f);
    *encoded = (uint64_t)scaled;
    return true;
}

/* ================= Storage Layer Abstraction ================= */

static inline uint32_t storage_sector_size(void)
{
    return BSP_SPIFlash_IsAvailable() ? SPI_ENERGY_FLASH_SECTOR_SIZE : INTERNAL_ENERGY_FLASH_PAGE_SIZE;
}

static inline uint32_t storage_sector_count(void)
{
    return BSP_SPIFlash_IsAvailable() ? SPI_ENERGY_FLASH_SECTOR_COUNT : INTERNAL_ENERGY_FLASH_PAGE_COUNT;
}

static inline uint32_t storage_base_addr(void)
{
    return BSP_SPIFlash_IsAvailable() ? SPI_ENERGY_FLASH_BASE : INTERNAL_ENERGY_FLASH_BASE;
}

static uint32_t record_address(uint32_t sector, uint32_t slot)
{
    return storage_base_addr() + sector * storage_sector_size() + slot * ENERGY_RECORD_LENGTH;
}

static bool read_record(uint32_t address, EnergyRecord_t *record)
{
    if (BSP_SPIFlash_IsAvailable()) {
        return BSP_SPIFlash_Read(address, (uint8_t *)record, sizeof(EnergyRecord_t));
    }
#ifdef CHARGE_ENERGY_STORAGE_HOST_TEST
    memcpy(record, &g_energy_test_flash[address - INTERNAL_ENERGY_FLASH_BASE], sizeof(EnergyRecord_t));
    return true;
#else
    const EnergyRecord_t *ptr = (const EnergyRecord_t *)(uintptr_t)address;
    memcpy(record, ptr, sizeof(EnergyRecord_t));
    return true;
#endif
}

static bool write_record(uint32_t address, uint32_t sequence,
                         float total_charged_ah, float total_energy_kwh,
                         uint32_t total_charge_seconds)
{
    EnergyRecord_t record = {0};
    uint64_t charged_x1000;
    uint64_t energy_x1000;
    if (!encode_counter(total_charged_ah, &charged_x1000) ||
        !encode_counter(total_energy_kwh, &energy_x1000)) {
        return false;
    }
    record.magic = ENERGY_RECORD_MAGIC;
    record.version = ENERGY_RECORD_VERSION_V2;
    record.length = ENERGY_RECORD_LENGTH_V2;
    record.sequence = sequence;
    record.total_charged_ah_x1000 = charged_x1000;
    record.total_energy_kwh_x1000 = energy_x1000;
    record.total_charge_seconds = total_charge_seconds;
    record.reserved = 0xFFFFFFFFU;
    record.crc32 = calc_crc32((const uint8_t *)&record.sequence, 28U);

    if (BSP_SPIFlash_IsAvailable()) {
        return BSP_SPIFlash_Write(address, (const uint8_t *)&record, sizeof(record));
    }
    return BSP_Flash_WriteBlock(address, (const uint8_t *)&record, sizeof(record));
}

static bool erase_sector(uint32_t sector)
{
    if (BSP_SPIFlash_IsAvailable()) {
        return BSP_SPIFlash_EraseSector4K(SPI_ENERGY_FLASH_BASE + sector * SPI_ENERGY_FLASH_SECTOR_SIZE);
    }
    return BSP_Flash_ErasePage(INTERNAL_ENERGY_FLASH_BASE + sector * INTERNAL_ENERGY_FLASH_PAGE_SIZE);
}

static bool is_slot_blank(uint32_t address)
{
    uint8_t raw[ENERGY_RECORD_LENGTH];
    if (BSP_SPIFlash_IsAvailable()) {
        if (!BSP_SPIFlash_Read(address, raw, ENERGY_RECORD_LENGTH)) {
            return false;
        }
    } else {
#ifdef CHARGE_ENERGY_STORAGE_HOST_TEST
        memcpy(raw, &g_energy_test_flash[address - INTERNAL_ENERGY_FLASH_BASE], ENERGY_RECORD_LENGTH);
#else
        memcpy(raw, (const void *)(uintptr_t)address, ENERGY_RECORD_LENGTH);
#endif
    }
    for (uint32_t i = 0U; i < ENERGY_RECORD_LENGTH; i++) {
        if (raw[i] != FLASH_BLANK_BYTE) return false;
    }
    return true;
}

/* ================= Load & Migration ================= */

static bool scan_legacy_v1_records(EnergyRecord_t *out_v2)
{
    bool have_v1 = false;
    EnergyRecordV1_t best_v1 = {0};
    uint32_t sec_count = storage_sector_count();
    uint32_t sec_size = storage_sector_size();
    uint32_t slots_v1 = sec_size / ENERGY_RECORD_LENGTH_V1;

    for (uint32_t sector = 0U; sector < sec_count; sector++) {
        for (uint32_t slot = 0U; slot < slots_v1; slot++) {
            uint32_t addr = storage_base_addr() + sector * sec_size + slot * ENERGY_RECORD_LENGTH_V1;
            EnergyRecordV1_t rec;
            bool read_ok = false;
            if (BSP_SPIFlash_IsAvailable()) {
                read_ok = BSP_SPIFlash_Read(addr, (uint8_t *)&rec, sizeof(rec));
            } else {
#ifdef CHARGE_ENERGY_STORAGE_HOST_TEST
                memcpy(&rec, &g_energy_test_flash[addr - INTERNAL_ENERGY_FLASH_BASE], sizeof(rec));
                read_ok = true;
#else
                const EnergyRecordV1_t *ptr = (const EnergyRecordV1_t *)(uintptr_t)addr;
                memcpy(&rec, ptr, sizeof(rec));
                read_ok = true;
#endif
            }
            if (read_ok && record_v1_valid(&rec)) {
                if (!have_v1 || sequence_newer(rec.sequence, best_v1.sequence)) {
                    best_v1 = rec;
                    have_v1 = true;
                }
            }
        }
    }

    if (have_v1 && out_v2 != NULL) {
        memset(out_v2, 0, sizeof(*out_v2));
        out_v2->magic = ENERGY_RECORD_MAGIC;
        out_v2->version = ENERGY_RECORD_VERSION_V2;
        out_v2->length = ENERGY_RECORD_LENGTH_V2;
        out_v2->sequence = best_v1.sequence;
        out_v2->total_charged_ah_x1000 = best_v1.total_charged_ah_x1000;
        out_v2->total_energy_kwh_x1000 = best_v1.total_energy_kwh_x1000;
        out_v2->total_charge_seconds = 0U;
        out_v2->reserved = 0xFFFFFFFFU;
    }
    return have_v1;
}

static bool scan_internal_flash_for_migration(EnergyRecord_t *out_record)
{
    bool have_record = false;
    EnergyRecord_t best = {0};

    /* First check V2 in internal flash */
    for (uint32_t page = 0U; page < INTERNAL_ENERGY_FLASH_PAGE_COUNT; page++) {
        for (uint32_t slot = 0U; slot < (INTERNAL_ENERGY_FLASH_PAGE_SIZE / ENERGY_RECORD_LENGTH_V2); slot++) {
            uint32_t addr = INTERNAL_ENERGY_FLASH_BASE + page * INTERNAL_ENERGY_FLASH_PAGE_SIZE + slot * ENERGY_RECORD_LENGTH_V2;
            const EnergyRecord_t *rec;
#ifdef CHARGE_ENERGY_STORAGE_HOST_TEST
            rec = (const EnergyRecord_t *)(const void *)&g_energy_test_flash[addr - INTERNAL_ENERGY_FLASH_BASE];
#else
            rec = (const EnergyRecord_t *)(const void *)(uintptr_t)addr;
#endif
            if (record_valid(rec) && (!have_record || sequence_newer(rec->sequence, best.sequence))) {
                best = *rec;
                have_record = true;
            }
        }
    }

    /* If no V2, check V1 in internal flash */
    if (!have_record) {
        bool have_v1 = false;
        EnergyRecordV1_t best_v1 = {0};
        for (uint32_t page = 0U; page < INTERNAL_ENERGY_FLASH_PAGE_COUNT; page++) {
            for (uint32_t slot = 0U; slot < (INTERNAL_ENERGY_FLASH_PAGE_SIZE / ENERGY_RECORD_LENGTH_V1); slot++) {
                uint32_t addr = INTERNAL_ENERGY_FLASH_BASE + page * INTERNAL_ENERGY_FLASH_PAGE_SIZE + slot * ENERGY_RECORD_LENGTH_V1;
                const EnergyRecordV1_t *rec;
#ifdef CHARGE_ENERGY_STORAGE_HOST_TEST
                rec = (const EnergyRecordV1_t *)(const void *)&g_energy_test_flash[addr - INTERNAL_ENERGY_FLASH_BASE];
#else
                rec = (const EnergyRecordV1_t *)(const void *)(uintptr_t)addr;
#endif
                if (record_v1_valid(rec) && (!have_v1 || sequence_newer(rec->sequence, best_v1.sequence))) {
                    best_v1 = *rec;
                    have_v1 = true;
                }
            }
        }
        if (have_v1) {
            memset(&best, 0, sizeof(best));
            best.magic = ENERGY_RECORD_MAGIC;
            best.version = ENERGY_RECORD_VERSION_V2;
            best.length = ENERGY_RECORD_LENGTH_V2;
            best.sequence = best_v1.sequence;
            best.total_charged_ah_x1000 = best_v1.total_charged_ah_x1000;
            best.total_energy_kwh_x1000 = best_v1.total_energy_kwh_x1000;
            best.total_charge_seconds = 0U;
            best.reserved = 0xFFFFFFFFU;
            have_record = true;
        }
    }

    if (have_record && out_record != NULL) {
        *out_record = best;
    }
    return have_record;
}

static void load_latest(void)
{
    bool have_latest = false;
    EnergyRecord_t latest = {0};
    uint32_t sec_count = storage_sector_count();
    uint32_t sec_size = storage_sector_size();
    uint32_t slots_per_sec = sec_size / ENERGY_RECORD_LENGTH;

    /* 1. Try reading active V2 records */
    for (uint32_t sector = 0U; sector < sec_count; sector++) {
        for (uint32_t slot = 0U; slot < slots_per_sec; slot++) {
            EnergyRecord_t record;
            if (read_record(record_address(sector, slot), &record)) {
                if (record_valid(&record) &&
                    (!have_latest || sequence_newer(record.sequence, latest.sequence))) {
                    latest = record;
                    have_latest = true;
                }
            }
        }
    }

    /* 2. If no V2 record found, check for legacy V1 records in active storage and migrate */
    if (!have_latest) {
        EnergyRecord_t v1_converted;
        if (scan_legacy_v1_records(&v1_converted)) {
            LOG("EnergyStorage: [MIGRATE] Found legacy V1 record (Ah=%.3f, kWh=%.3f)! Upgrading to V2...\r\n",
                (float)(uint32_t)v1_converted.total_charged_ah_x1000 / ENERGY_SCALE,
                (float)(uint32_t)v1_converted.total_energy_kwh_x1000 / ENERGY_SCALE);
            erase_sector(0U);
            float ah = (float)(uint32_t)v1_converted.total_charged_ah_x1000 / ENERGY_SCALE;
            float kwh = (float)(uint32_t)v1_converted.total_energy_kwh_x1000 / ENERGY_SCALE;
            if (write_record(record_address(0U, 0U), v1_converted.sequence + 1U, ah, kwh, 0U)) {
                latest = v1_converted;
                latest.sequence++;
                have_latest = true;
            }
        }
    }

    /* 3. If SPI Flash is active but empty, migrate from Internal Flash */
    if (!have_latest && BSP_SPIFlash_IsAvailable()) {
        LOG("EnergyStorage: External Flash journal empty, checking Internal Flash for migration...\r\n");
        EnergyRecord_t internal_record;
        if (scan_internal_flash_for_migration(&internal_record)) {
            LOG("EnergyStorage: [MIGRATE] Found legacy counters in Internal Flash (Ah=%.3f, kWh=%.3f)! Migrating to External SPI Flash...\r\n",
                (float)(uint32_t)internal_record.total_charged_ah_x1000 / ENERGY_SCALE,
                (float)(uint32_t)internal_record.total_energy_kwh_x1000 / ENERGY_SCALE);
            erase_sector(0U);
            float ah = (float)(uint32_t)internal_record.total_charged_ah_x1000 / ENERGY_SCALE;
            float kwh = (float)(uint32_t)internal_record.total_energy_kwh_x1000 / ENERGY_SCALE;
            uint32_t sec = internal_record.total_charge_seconds;
            if (write_record(record_address(0U, 0U), internal_record.sequence + 1U, ah, kwh, sec)) {
                latest = internal_record;
                latest.sequence++;
                have_latest = true;
            }
        }
    }

    if (have_latest) {
        uint32_t chg_32 = (uint32_t)latest.total_charged_ah_x1000;
        uint32_t nrg_32 = (uint32_t)latest.total_energy_kwh_x1000;
        g_total_charged_ah = (float)chg_32 / ENERGY_SCALE;
        g_total_energy_kwh = (float)nrg_32 / ENERGY_SCALE;
        g_total_charge_seconds = latest.total_charge_seconds;
        g_last_saved_charged_x1000 = latest.total_charged_ah_x1000;
        g_last_saved_energy_x1000 = latest.total_energy_kwh_x1000;
        g_last_saved_seconds = latest.total_charge_seconds;
        g_have_saved_record = true;
        g_next_sequence = latest.sequence + 1U;
        LOG("EnergyStorage: Loaded seq=%lu Ah=%.3f kWh=%.3f Sec=%lu from %s\r\n",
            (unsigned long)latest.sequence, g_total_charged_ah, g_total_energy_kwh,
            (unsigned long)g_total_charge_seconds,
            BSP_SPIFlash_IsAvailable() ? "External SPI Flash" : "Internal Flash");
    } else {
        g_total_charged_ah = 0.0f;
        g_total_energy_kwh = 0.0f;
        g_total_charge_seconds = 0U;
        g_last_saved_charged_x1000 = 0U;
        g_last_saved_energy_x1000 = 0U;
        g_last_saved_seconds = 0U;
        g_have_saved_record = false;
        g_next_sequence = 0U;
        LOG("EnergyStorage: No valid record on %s, initialized to zero\r\n",
            BSP_SPIFlash_IsAvailable() ? "External SPI Flash" : "Internal Flash");
    }
}

/* ================= Public API ================= */

void ChargeEnergyStorage_Init(void)
{
    if (g_initialized) return;
    load_latest();
    g_was_charging = false;
    g_last_checkpoint_tick = 0U;
    g_initialized = true;
}

void ChargeEnergyStorage_Get(float *total_charged_ah,
                             float *total_energy_kwh,
                             uint32_t *total_charge_seconds)
{
    if (total_charged_ah != NULL) *total_charged_ah = g_total_charged_ah;
    if (total_energy_kwh != NULL) *total_energy_kwh = g_total_energy_kwh;
    if (total_charge_seconds != NULL) *total_charge_seconds = g_total_charge_seconds;
}

void ChargeEnergyStorage_SaveNow(float total_charged_ah,
                                 float total_energy_kwh,
                                 uint32_t total_charge_seconds)
{
    if (!g_initialized || !isfinite(total_charged_ah) ||
        !isfinite(total_energy_kwh) || total_charged_ah < 0.0f ||
        total_energy_kwh < 0.0f) {
        LOG("EnergyStorage: save rejected (invalid state/value)\r\n");
        return;
    }

    uint64_t charged_x1000;
    uint64_t energy_x1000;
    if (!encode_counter(total_charged_ah, &charged_x1000) ||
        !encode_counter(total_energy_kwh, &energy_x1000)) {
        LOG("EnergyStorage: save rejected (counter overflow)\r\n");
        return;
    }
    if (g_have_saved_record && charged_x1000 == g_last_saved_charged_x1000 &&
        energy_x1000 == g_last_saved_energy_x1000 &&
        total_charge_seconds == g_last_saved_seconds) {
        return;
    }

    uint32_t sec_count = storage_sector_count();
    uint32_t sec_size = storage_sector_size();
    uint32_t slots_per_sec = sec_size / ENERGY_RECORD_LENGTH;

    /* Append to first blank slot */
    for (uint32_t sector = 0U; sector < sec_count; sector++) {
        for (uint32_t slot = 0U; slot < slots_per_sec; slot++) {
            uint32_t address = record_address(sector, slot);
            if (is_slot_blank(address)) {
                if (write_record(address, g_next_sequence++, total_charged_ah,
                                 total_energy_kwh, total_charge_seconds)) {
                    g_total_charged_ah = total_charged_ah;
                    g_total_energy_kwh = total_energy_kwh;
                    g_total_charge_seconds = total_charge_seconds;
                    g_last_saved_charged_x1000 = charged_x1000;
                    g_last_saved_energy_x1000 = energy_x1000;
                    g_last_saved_seconds = total_charge_seconds;
                    g_have_saved_record = true;
                    LOG("EnergyStorage: checkpoint saved to %s (sec=%lu slot=%lu)\r\n",
                        BSP_SPIFlash_IsAvailable() ? "External SPI Flash" : "Internal Flash",
                        (unsigned long)sector, (unsigned long)slot);
                } else {
                    LOG("EnergyStorage: checkpoint write failed\r\n");
                }
                return;
            }
        }
    }

    /* All sectors full -> rotate oldest sector */
    bool have_oldest = false;
    uint32_t oldest_sector = 0U;
    uint32_t oldest_sequence = 0U;
    for (uint32_t sector = 0U; sector < sec_count; sector++) {
        for (uint32_t slot = 0U; slot < slots_per_sec; slot++) {
            EnergyRecord_t record;
            if (read_record(record_address(sector, slot), &record)) {
                if (record_valid(&record) &&
                    (!have_oldest || sequence_newer(oldest_sequence, record.sequence))) {
                    oldest_sector = sector;
                    oldest_sequence = record.sequence;
                    have_oldest = true;
                }
            }
        }
    }

    if (!have_oldest || !erase_sector(oldest_sector) ||
        !write_record(record_address(oldest_sector, 0U), g_next_sequence++,
                      total_charged_ah, total_energy_kwh, total_charge_seconds)) {
        LOG("EnergyStorage: journal rotation failed\r\n");
        return;
    }

    g_total_charged_ah = total_charged_ah;
    g_total_energy_kwh = total_energy_kwh;
    g_total_charge_seconds = total_charge_seconds;
    g_last_saved_charged_x1000 = charged_x1000;
    g_last_saved_energy_x1000 = energy_x1000;
    g_last_saved_seconds = total_charge_seconds;
    g_have_saved_record = true;
    LOG("EnergyStorage: journal rotated sector %lu on %s\r\n",
        (unsigned long)oldest_sector,
        BSP_SPIFlash_IsAvailable() ? "External SPI Flash" : "Internal Flash");
}

void ChargeEnergyStorage_Process(uint32_t now_tick, bool charging,
                                 float total_charged_ah,
                                 float total_energy_kwh,
                                 uint32_t total_charge_seconds)
{
    if (!g_initialized) return;
    if (charging) {
        if (!g_was_charging) {
            g_was_charging = true;
            g_last_checkpoint_tick = now_tick;
            return;
        }
        if ((uint32_t)(now_tick - g_last_checkpoint_tick) >= ENERGY_CHECKPOINT_MS) {
            ChargeEnergyStorage_SaveNow(total_charged_ah, total_energy_kwh, total_charge_seconds);
            g_last_checkpoint_tick = now_tick;
        }
    } else if (g_was_charging) {
        ChargeEnergyStorage_SaveNow(total_charged_ah, total_energy_kwh, total_charge_seconds);
        g_was_charging = false;
    }
}

bool ChargeEnergyStorage_Reset(void)
{
    if (!g_initialized) return false;
    uint32_t sec_count = storage_sector_count();
    for (uint32_t sec = 0U; sec < sec_count; sec++) {
        if (!erase_sector(sec)) {
            LOG("EnergyStorage: reset erase failed at sector %lu\r\n", (unsigned long)sec);
            return false;
        }
    }
    g_total_charged_ah = 0.0f;
    g_total_energy_kwh = 0.0f;
    g_total_charge_seconds = 0U;
    g_last_saved_charged_x1000 = 0U;
    g_last_saved_energy_x1000 = 0U;
    g_last_saved_seconds = 0U;
    g_have_saved_record = false;
    g_next_sequence = 0U;
    g_reset_request = true;
    LOG("EnergyStorage: counters reset on %s\r\n",
        BSP_SPIFlash_IsAvailable() ? "External SPI Flash" : "Internal Flash");
    return true;
}

bool ChargeEnergyStorage_TakeResetRequest(void)
{
    bool requested = g_reset_request;
    g_reset_request = false;
    return requested;
}
