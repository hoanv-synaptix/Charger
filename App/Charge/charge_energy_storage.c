/**
 * @file charge_energy_storage.c
 * @brief Four-page journal for accumulated charge and energy counters.
 */

#include "charge_energy_storage.h"
#include "bsp_flash.h"
#include "debug_log.h"

#include <math.h>
#include <string.h>

#define ENERGY_FLASH_BASE       0x0801D800U /* pages 59..62 */
#define ENERGY_FLASH_PAGE_SIZE  BSP_FLASH_PAGE_SIZE
#define ENERGY_FLASH_PAGE_COUNT 4U
#define ENERGY_RECORD_MAGIC     0x454E4731U /* "ENG1" */
#define ENERGY_RECORD_VERSION   1U
#define ENERGY_RECORD_LENGTH    32U
#define ENERGY_CHECKPOINT_MS    300000U
#define ENERGY_SCALE            1000.0f
#define FLASH_BLANK_BYTE        0xFFU

#ifdef CHARGE_ENERGY_STORAGE_HOST_TEST
extern uint8_t g_energy_test_flash[];
#endif

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t length;
    uint32_t sequence;
    uint64_t total_charged_ah_x1000;
    uint64_t total_energy_kwh_x1000;
    uint32_t crc32;
} EnergyRecord_t;

_Static_assert(sizeof(EnergyRecord_t) == ENERGY_RECORD_LENGTH,
               "EnergyRecord_t must stay 32 bytes");

static float g_total_charged_ah;
static float g_total_energy_kwh;
static uint64_t g_last_saved_charged_x1000;
static uint64_t g_last_saved_energy_x1000;
static bool g_have_saved_record;
static uint32_t g_next_sequence;
static bool g_initialized;
static bool g_was_charging;
static bool g_reset_request;
static uint32_t g_last_checkpoint_tick;

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

static uint32_t record_address(uint32_t page, uint32_t slot)
{
    return ENERGY_FLASH_BASE + page * ENERGY_FLASH_PAGE_SIZE +
           slot * ENERGY_RECORD_LENGTH;
}

#ifdef CHARGE_ENERGY_STORAGE_HOST_TEST
static uint8_t *raw_at(uint32_t address)
{
    return &g_energy_test_flash[address - ENERGY_FLASH_BASE];
}
#else
static uint8_t *raw_at(uint32_t address)
{
    return (uint8_t *)(uintptr_t)address;
}
#endif

static const EnergyRecord_t *record_at(uint32_t address)
{
    return (const EnergyRecord_t *)(const void *)raw_at(address);
}

static bool record_valid(const EnergyRecord_t *record)
{
    if (record == NULL || record->magic != ENERGY_RECORD_MAGIC ||
        record->version != ENERGY_RECORD_VERSION ||
        record->length != ENERGY_RECORD_LENGTH) {
        return false;
    }
    return record->crc32 == calc_crc32((const uint8_t *)&record->sequence, 20U);
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

static bool write_record(uint32_t address, uint32_t sequence,
                         float total_charged_ah, float total_energy_kwh)
{
    EnergyRecord_t record = {0};
    uint64_t charged_x1000;
    uint64_t energy_x1000;
    if (!encode_counter(total_charged_ah, &charged_x1000) ||
        !encode_counter(total_energy_kwh, &energy_x1000)) {
        return false;
    }
    record.magic = ENERGY_RECORD_MAGIC;
    record.version = ENERGY_RECORD_VERSION;
    record.length = ENERGY_RECORD_LENGTH;
    record.sequence = sequence;
    record.total_charged_ah_x1000 = charged_x1000;
    record.total_energy_kwh_x1000 = energy_x1000;
    record.crc32 = calc_crc32((const uint8_t *)&record.sequence, 20U);
    return BSP_Flash_WriteBlock(address, (const uint8_t *)&record,
                                sizeof(record));
}

static void load_latest(void)
{
    bool have_latest = false;
    EnergyRecord_t latest = {0};
    for (uint32_t page = 0U; page < ENERGY_FLASH_PAGE_COUNT; page++) {
        for (uint32_t slot = 0U;
             slot < (ENERGY_FLASH_PAGE_SIZE / ENERGY_RECORD_LENGTH); slot++) {
            const EnergyRecord_t *record = record_at(record_address(page, slot));
            if (record_valid(record) &&
                (!have_latest || sequence_newer(record->sequence, latest.sequence))) {
                latest = *record;
                have_latest = true;
            }
        }
    }
    if (have_latest) {
        uint32_t chg_32 = (uint32_t)latest.total_charged_ah_x1000;
        uint32_t nrg_32 = (uint32_t)latest.total_energy_kwh_x1000;
        g_total_charged_ah = (float)chg_32 / ENERGY_SCALE;
        g_total_energy_kwh = (float)nrg_32 / ENERGY_SCALE;
        g_last_saved_charged_x1000 = latest.total_charged_ah_x1000;
        g_last_saved_energy_x1000 = latest.total_energy_kwh_x1000;
        g_have_saved_record = true;
        g_next_sequence = latest.sequence + 1U;
        LOG("EnergyStorage: loaded seq=%lu Ah=%.3f kWh=%.3f\r\n",
            (unsigned long)latest.sequence, g_total_charged_ah, g_total_energy_kwh);
    } else {
        g_total_charged_ah = 0.0f;
        g_total_energy_kwh = 0.0f;
        g_last_saved_charged_x1000 = 0U;
        g_last_saved_energy_x1000 = 0U;
        g_have_saved_record = false;
        g_next_sequence = 0U;
        LOG("EnergyStorage: no valid record, counters reset to zero\r\n");
    }
}

void ChargeEnergyStorage_Init(void)
{
    if (g_initialized) return;
    load_latest();
    g_was_charging = false;
    g_last_checkpoint_tick = 0U;
    g_initialized = true;
}

void ChargeEnergyStorage_Get(float *total_charged_ah,
                             float *total_energy_kwh)
{
    if (total_charged_ah != NULL) *total_charged_ah = g_total_charged_ah;
    if (total_energy_kwh != NULL) *total_energy_kwh = g_total_energy_kwh;
}

void ChargeEnergyStorage_SaveNow(float total_charged_ah,
                                 float total_energy_kwh)
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
        energy_x1000 == g_last_saved_energy_x1000) {
        return;
    }

    for (uint32_t page = 0U; page < ENERGY_FLASH_PAGE_COUNT; page++) {
        for (uint32_t slot = 0U;
             slot < (ENERGY_FLASH_PAGE_SIZE / ENERGY_RECORD_LENGTH); slot++) {
            uint32_t address = record_address(page, slot);
            const uint8_t *raw = raw_at(address);
            bool blank = true;
            for (uint32_t i = 0U; i < ENERGY_RECORD_LENGTH; i++) {
                if (raw[i] != FLASH_BLANK_BYTE) { blank = false; break; }
            }
            if (blank) {
                if (write_record(address, g_next_sequence++, total_charged_ah,
                                 total_energy_kwh)) {
                    g_total_charged_ah = total_charged_ah;
                    g_total_energy_kwh = total_energy_kwh;
                    g_last_saved_charged_x1000 = charged_x1000;
                    g_last_saved_energy_x1000 = energy_x1000;
                    g_have_saved_record = true;
                    LOG("EnergyStorage: checkpoint saved\r\n");
                } else {
                    LOG("EnergyStorage: checkpoint write failed\r\n");
                }
                return;
            }
        }
    }

    bool have_oldest = false;
    uint32_t oldest_page = 0U;
    uint32_t oldest_sequence = 0U;
    for (uint32_t page = 0U; page < ENERGY_FLASH_PAGE_COUNT; page++) {
        for (uint32_t slot = 0U;
             slot < (ENERGY_FLASH_PAGE_SIZE / ENERGY_RECORD_LENGTH); slot++) {
            const EnergyRecord_t *record = record_at(record_address(page, slot));
            if (record_valid(record) &&
                (!have_oldest || sequence_newer(oldest_sequence, record->sequence))) {
                oldest_page = page;
                oldest_sequence = record->sequence;
                have_oldest = true;
            }
        }
    }
    if (!have_oldest || !BSP_Flash_ErasePage(ENERGY_FLASH_BASE +
                                              oldest_page * ENERGY_FLASH_PAGE_SIZE) ||
        !write_record(record_address(oldest_page, 0U), g_next_sequence++,
                      total_charged_ah, total_energy_kwh)) {
        LOG("EnergyStorage: journal rotation failed\r\n");
        return;
    }
    g_total_charged_ah = total_charged_ah;
    g_total_energy_kwh = total_energy_kwh;
    g_last_saved_charged_x1000 = charged_x1000;
    g_last_saved_energy_x1000 = energy_x1000;
    g_have_saved_record = true;
    LOG("EnergyStorage: journal rotated and checkpoint saved\r\n");
}

void ChargeEnergyStorage_Process(uint32_t now_tick, bool charging,
                                 float total_charged_ah,
                                 float total_energy_kwh)
{
    if (!g_initialized) return;
    if (charging) {
        if (!g_was_charging) {
            g_was_charging = true;
            g_last_checkpoint_tick = now_tick;
            return;
        }
        if ((uint32_t)(now_tick - g_last_checkpoint_tick) >= ENERGY_CHECKPOINT_MS) {
            ChargeEnergyStorage_SaveNow(total_charged_ah, total_energy_kwh);
            g_last_checkpoint_tick = now_tick;
        }
    } else if (g_was_charging) {
        ChargeEnergyStorage_SaveNow(total_charged_ah, total_energy_kwh);
        g_was_charging = false;
    }
}

bool ChargeEnergyStorage_Reset(void)
{
    if (!g_initialized) return false;
    for (uint32_t page = 0U; page < ENERGY_FLASH_PAGE_COUNT; page++) {
        if (!BSP_Flash_ErasePage(ENERGY_FLASH_BASE + page * ENERGY_FLASH_PAGE_SIZE)) {
            LOG("EnergyStorage: reset erase failed at page %lu\r\n",
                (unsigned long)page);
            return false;
        }
    }
    g_total_charged_ah = 0.0;
    g_total_energy_kwh = 0.0;
    g_last_saved_charged_x1000 = 0U;
    g_last_saved_energy_x1000 = 0U;
    g_have_saved_record = false;
    g_next_sequence = 0U;
    g_reset_request = true;
    LOG("EnergyStorage: counters reset\r\n");
    return true;
}

bool ChargeEnergyStorage_TakeResetRequest(void)
{
    bool requested = g_reset_request;
    g_reset_request = false;
    return requested;
}
