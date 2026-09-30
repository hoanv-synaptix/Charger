/**
 * @file test_energy_storage.c
 * @brief Host regression tests for the persistent energy journal policy (V2 with total_charge_seconds).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bsp_flash.h"
#include "charge_energy_storage.h"

#define TEST_FLASH_SIZE (4U * BSP_FLASH_PAGE_SIZE)
#define TEST_BASE       0x0801D800U

uint8_t g_energy_test_flash[TEST_FLASH_SIZE];
static uint32_t g_write_count;
static uint32_t g_erase_count;

void LOG(const char *fmt, ...)
{
    (void)fmt;
}

bool BSP_SPIFlash_IsAvailable(void) { return false; }
bool BSP_SPIFlash_Read(uint32_t a, uint8_t *b, uint32_t l) { (void)a; (void)b; (void)l; return false; }
bool BSP_SPIFlash_Write(uint32_t a, const uint8_t *d, uint32_t l) { (void)a; (void)d; (void)l; return false; }
bool BSP_SPIFlash_EraseSector4K(uint32_t a) { (void)a; return false; }

bool BSP_Flash_ErasePage(uint32_t address)
{
    if (address < TEST_BASE || address >= TEST_BASE + TEST_FLASH_SIZE ||
        ((address - TEST_BASE) % BSP_FLASH_PAGE_SIZE) != 0U) {
        return false;
    }
    memset(&g_energy_test_flash[address - TEST_BASE], 0xFF, BSP_FLASH_PAGE_SIZE);
    g_erase_count++;
    return true;
}

bool BSP_Flash_WriteBlock(uint32_t address, const uint8_t *data, uint32_t len)
{
    if (data == NULL || address < TEST_BASE ||
        address + len > TEST_BASE + TEST_FLASH_SIZE || (len % 8U) != 0U) {
        return false;
    }
    memcpy(&g_energy_test_flash[address - TEST_BASE], data, len);
    g_write_count++;
    return true;
}

static bool expect(bool condition, const char *name)
{
    if (!condition) {
        printf("[FAIL] %s\n", name);
        return false;
    }
    return true;
}

static uint32_t test_calc_crc32(const uint8_t *data, uint32_t len)
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

int main(void)
{
    float ah = 0.0f;
    float kwh = 0.0f;
    uint32_t sec = 0U;
    bool ok = true;

    memset(g_energy_test_flash, 0xFF, sizeof(g_energy_test_flash));
    ChargeEnergyStorage_Init();
    ChargeEnergyStorage_Get(&ah, &kwh, &sec);
    ok &= expect(ah == 0.0f && kwh == 0.0f && sec == 0U, "blank flash loads zero");

    ChargeEnergyStorage_SaveNow(1.234f, 5.678f, 3600U);
    ChargeEnergyStorage_Get(&ah, &kwh, &sec);
    ok &= expect(ah == 1.234f && kwh == 5.678f && sec == 3600U, "save updates RAM counters with seconds");
    uint32_t writes_after_first = g_write_count;
    ChargeEnergyStorage_SaveNow(1.234f, 5.678f, 3600U);
    ok &= expect(g_write_count == writes_after_first, "unchanged values are not rewritten");

    ChargeEnergyStorage_Process(0U, true, 2.0f, 6.0f, 7200U);
    ChargeEnergyStorage_Process(299999U, true, 2.0f, 6.0f, 7200U);
    ok &= expect(g_write_count == writes_after_first, "checkpoint waits five minutes");
    ChargeEnergyStorage_Process(300000U, true, 2.0f, 6.0f, 7200U);
    ok &= expect(g_write_count == writes_after_first + 1U, "checkpoint writes at five minutes");
    ChargeEnergyStorage_Process(300001U, false, 2.5f, 7.0f, 7201U);
    ChargeEnergyStorage_Get(&ah, &kwh, &sec);
    ok &= expect(ah == 2.5f && kwh == 7.0f && sec == 7201U, "stop writes final counters");

    ok &= expect(ChargeEnergyStorage_Reset(), "reset succeeds");
    ChargeEnergyStorage_Get(&ah, &kwh, &sec);
    ok &= expect(ah == 0.0f && kwh == 0.0f && sec == 0U && g_erase_count == 4U,
                 "reset clears RAM and all journal pages");

    /* Test Legacy V1 record migration */
    struct __attribute__((packed)) {
        uint32_t magic;
        uint16_t version;
        uint16_t length;
        uint32_t sequence;
        uint64_t total_charged_ah_x1000;
        uint64_t total_energy_kwh_x1000;
        uint32_t crc32;
    } legacy_v1;

    memset(g_energy_test_flash, 0xFF, sizeof(g_energy_test_flash));
    legacy_v1.magic = 0x454E4731U;
    legacy_v1.version = 1U;
    legacy_v1.length = 32U;
    legacy_v1.sequence = 42U;
    legacy_v1.total_charged_ah_x1000 = (uint64_t)(10.5f * 1000.0f + 0.5f);
    legacy_v1.total_energy_kwh_x1000 = (uint64_t)(25.2f * 1000.0f + 0.5f);
    legacy_v1.crc32 = test_calc_crc32((const uint8_t *)&legacy_v1.sequence, 20U);
    memcpy(&g_energy_test_flash[0], &legacy_v1, sizeof(legacy_v1));

    /* Reset internal storage state to force re-init */
    void ChargeEnergyStorage_TestResetInternalState(void);
    ChargeEnergyStorage_TestResetInternalState();
    ChargeEnergyStorage_Init();
    ChargeEnergyStorage_Get(&ah, &kwh, &sec);
    ok &= expect(ah == 10.5f && kwh == 25.2f && sec == 0U,
                 "legacy v1 record migrated cleanly with sec=0");

    printf(ok ? "ALL ENERGY STORAGE TESTS PASSED.\n"
              : "ENERGY STORAGE TESTS FAILED.\n");
    return ok ? 0 : 1;
}
