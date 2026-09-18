/**
 * @file test_energy_storage.c
 * @brief Host regression tests for the persistent energy journal policy.
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

int main(void)
{
    float ah = 0.0f;
    float kwh = 0.0f;
    bool ok = true;

    memset(g_energy_test_flash, 0xFF, sizeof(g_energy_test_flash));
    ChargeEnergyStorage_Init();
    ChargeEnergyStorage_Get(&ah, &kwh);
    ok &= expect(ah == 0.0f && kwh == 0.0f, "blank flash loads zero");

    ChargeEnergyStorage_SaveNow(1.234f, 5.678f);
    ChargeEnergyStorage_Get(&ah, &kwh);
    ok &= expect(ah == 1.234f && kwh == 5.678f, "save updates RAM counters");
    uint32_t writes_after_first = g_write_count;
    ChargeEnergyStorage_SaveNow(1.234f, 5.678f);
    ok &= expect(g_write_count == writes_after_first, "unchanged values are not rewritten");

    ChargeEnergyStorage_Process(0U, true, 2.0f, 6.0f);
    ChargeEnergyStorage_Process(299999U, true, 2.0f, 6.0f);
    ok &= expect(g_write_count == writes_after_first, "checkpoint waits five minutes");
    ChargeEnergyStorage_Process(300000U, true, 2.0f, 6.0f);
    ok &= expect(g_write_count == writes_after_first + 1U, "checkpoint writes at five minutes");
    ChargeEnergyStorage_Process(300001U, false, 2.5f, 7.0f);
    ChargeEnergyStorage_Get(&ah, &kwh);
    ok &= expect(ah == 2.5f && kwh == 7.0f, "stop writes final counters");

    ok &= expect(ChargeEnergyStorage_Reset(), "reset succeeds");
    ChargeEnergyStorage_Get(&ah, &kwh);
    ok &= expect(ah == 0.0f && kwh == 0.0f && g_erase_count == 4U,
                 "reset clears RAM and all journal pages");

    printf(ok ? "ALL ENERGY STORAGE TESTS PASSED.\n"
              : "ENERGY STORAGE TESTS FAILED.\n");
    return ok ? 0 : 1;
}
