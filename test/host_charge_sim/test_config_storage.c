/* Host regression for ChargeCycleConfig v5 -> v6 Flash migration. */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bsp_flash.h"
#include "charge_cycle_config.h"
#include "charge_cycle_storage.h"

uint8_t g_charge_config_test_flash[BSP_FLASH_PAGE_SIZE];

void LOG(const char *fmt, ...) { (void)fmt; }
void BSP_EnterCritical(void) {}
void BSP_ExitCritical(void) {}

bool BSP_Flash_ErasePage(uint32_t address)
{
    if (address != BSP_CONFIG_FLASH_PAGE_ADDR) return false;
    memset(g_charge_config_test_flash, 0xFF, sizeof(g_charge_config_test_flash));
    return true;
}

bool BSP_Flash_WriteBlock(uint32_t address, const uint8_t *data, uint32_t len)
{
    if (data == NULL || address < BSP_CONFIG_FLASH_PAGE_ADDR ||
        address + len > BSP_CONFIG_FLASH_PAGE_ADDR + BSP_FLASH_PAGE_SIZE) {
        return false;
    }
    memcpy(&g_charge_config_test_flash[address - BSP_CONFIG_FLASH_PAGE_ADDR], data, len);
    return true;
}

static uint32_t crc32(const uint8_t *data, uint32_t len)
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

static bool expect(bool condition, const char *message)
{
    if (!condition) {
        printf("[FAIL] %s\n", message);
        return false;
    }
    return true;
}

int main(void)
{
    bool ok = true;
    ChargeCycleConfig_t old_cfg;
    ChargeCycleConfig_t loaded;
    uint8_t record[256]; /* v5 and v6 records both occupy one aligned slot. */

    memset(g_charge_config_test_flash, 0xFF, sizeof(g_charge_config_test_flash));
    ChargeCycleConfig_GetDefaults(&old_cfg);
    old_cfg.version = 5U;
    old_cfg.vlow_v = 47.5f;
    old_cfg.ilow_c = 0.35f;

    memset(record, 0xFF, sizeof(record));
    /* {magic, record_version, payload_length, crc32, v5 payload[239]}. */
    uint32_t magic = 0x43434647U;
    uint16_t record_version = 1U;
    uint16_t payload_length = 239U;
    memcpy(&record[0], &magic, sizeof(magic));
    memcpy(&record[4], &record_version, sizeof(record_version));
    memcpy(&record[6], &payload_length, sizeof(payload_length));
    memcpy(&record[12], &old_cfg, 239U);
    uint32_t payload_crc = crc32(&record[12], 239U);
    memcpy(&record[8], &payload_crc, sizeof(payload_crc));
    memcpy(g_charge_config_test_flash, record, sizeof(record));

    ok &= expect(ChargeCycleStorage_Load(&loaded), "v5 record loads");
    ok &= expect(loaded.version == CHARGE_CYCLE_CONFIG_VERSION, "v5 migration upgrades version to v7");
    ok &= expect(loaded.admin_pin == DEFAULT_ADMIN_PIN, "v5 migration installs default PIN");
    ok &= expect(loaded.vlow_v == old_cfg.vlow_v && loaded.ilow_c == old_cfg.ilow_c,
                 "v5 migration preserves v5 configuration fields");
    ok &= expect(loaded.charge_mode == 0U && loaded.delay_enabled == 0U &&
                 loaded.delay_hours == 2U && loaded.delay_minutes == 30U,
                 "v5 migration installs v7 defaults (FAST, DELAY OFF, 02:30)");

    /* Test v6 -> v7 migration (v6 payload = 243 bytes, includes custom admin_pin) */
    ChargeCycleConfig_GetDefaults(&old_cfg);
    old_cfg.version = 6U;
    old_cfg.admin_pin = 9999U;
    memset(record, 0xFF, sizeof(record));
    payload_length = 243U;
    memcpy(&record[0], &magic, sizeof(magic));
    memcpy(&record[4], &record_version, sizeof(record_version));
    memcpy(&record[6], &payload_length, sizeof(payload_length));
    memcpy(&record[12], &old_cfg, 243U);
    payload_crc = crc32(&record[12], 243U);
    memcpy(&record[8], &payload_crc, sizeof(payload_crc));
    memcpy(g_charge_config_test_flash, record, sizeof(record));

    ok &= expect(ChargeCycleStorage_Load(&loaded), "v6 record loads");
    ok &= expect(loaded.version == CHARGE_CYCLE_CONFIG_VERSION, "v6 migration upgrades version to v7");
    ok &= expect(loaded.admin_pin == 9999U, "v6 migration preserves custom admin PIN");
    ok &= expect(loaded.charge_mode == 0U && loaded.delay_enabled == 0U &&
                 loaded.delay_hours == 2U && loaded.delay_minutes == 30U,
                 "v6 migration installs v7 defaults (FAST, DELAY OFF, 02:30)");

    /* Test native v7 save and load roundtrip */
    ChargeCycleConfig_GetDefaults(&loaded);
    loaded.charge_mode = 1U;
    loaded.delay_enabled = 1U;
    loaded.delay_hours = 5U;
    loaded.delay_minutes = 45U;
    ok &= expect(ChargeCycleStorage_Save(&loaded), "v7 native save succeeds");

    ChargeCycleConfig_t reloaded;
    memset(&reloaded, 0, sizeof(reloaded));
    ok &= expect(ChargeCycleStorage_Load(&reloaded), "v7 native load succeeds");
    ok &= expect(reloaded.version == CHARGE_CYCLE_CONFIG_VERSION, "v7 native load version matches");
    ok &= expect(reloaded.charge_mode == 1U && reloaded.delay_enabled == 1U &&
                 reloaded.delay_hours == 5U && reloaded.delay_minutes == 45U,
                 "v7 native fields roundtrip intact");

    printf(ok ? "ALL CONFIG STORAGE TESTS PASSED.\n"
              : "CONFIG STORAGE TESTS FAILED.\n");
    return ok ? 0 : 1;
}
