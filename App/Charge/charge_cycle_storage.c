/**
 * @file charge_cycle_storage.c
 * @brief Persistent storage for ChargeCycleConfig_t using External SPI Flash
 *        with automatic Migration from Internal Flash.
 */

#include "charge_cycle_storage.h"
#include "debug_log.h"
#include "bsp_spi_flash.h"
#include "bsp_flash.h"
#include <string.h>
#include <stddef.h>

#define CONFIG_MAGIC            0x43434647U
#define CONFIG_RECORD_VERSION   1U
#define FLASH_BLANK_BYTE        0xFFU
#define CONFIG_V5_PAYLOAD_SIZE  239U
#define CONFIG_V6_PAYLOAD_SIZE  243U
#define CONFIG_V7_PAYLOAD_SIZE  249U
#define CONFIG_V8_PAYLOAD_SIZE  253U

#ifdef CHARGE_CYCLE_STORAGE_HOST_TEST
extern uint8_t g_charge_config_test_flash[];
#endif

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t length;
    uint32_t crc32;
    ChargeCycleConfig_t payload;
} ChargeCycleConfigRecord_t;

#define CONFIG_RECORD_SIZE     (sizeof(ChargeCycleConfigRecord_t))
#define ALIGNED_RECORD_SIZE    ((CONFIG_RECORD_SIZE + 7) & ~7) // Align to 8 bytes / 256 bytes

_Static_assert(offsetof(ChargeCycleConfig_t, admin_pin) == CONFIG_V5_PAYLOAD_SIZE,
               "v6 must append admin_pin after the v5 payload");
_Static_assert(offsetof(ChargeCycleConfig_t, charge_mode) == CONFIG_V6_PAYLOAD_SIZE,
               "v7 must append charge_mode after the v6 payload");
_Static_assert(offsetof(ChargeCycleConfig_t, imax_a) == CONFIG_V7_PAYLOAD_SIZE,
               "v8 must append imax_a after the v7 payload");
_Static_assert(offsetof(ChargeCycleConfig_t, module_address) == CONFIG_V8_PAYLOAD_SIZE,
               "v9 must append module_address after the v8 payload");

static const uint8_t *internal_flash_at(uint32_t address)
{
#ifdef CHARGE_CYCLE_STORAGE_HOST_TEST
    return &g_charge_config_test_flash[address - BSP_CONFIG_FLASH_PAGE_ADDR];
#else
    return (const uint8_t *)(uintptr_t)address;
#endif
}

static uint32_t calc_crc32(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFU;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 1U) crc = (crc >> 1U) ^ 0xEDB88320U;
            else crc >>= 1U;
        }
    }
    return ~crc;
}

static bool validate_record_length(const ChargeCycleConfigRecord_t *rec, uint16_t length) {
    if (rec->magic != CONFIG_MAGIC) return false;
    if (rec->version != CONFIG_RECORD_VERSION) return false;
    if (rec->length != length) return false;

    uint32_t calc_crc = calc_crc32((const uint8_t *)&rec->payload, length);
    return rec->crc32 == calc_crc;
}

static bool validate_record(const ChargeCycleConfigRecord_t *rec) {
    return validate_record_length(rec, sizeof(ChargeCycleConfig_t));
}

static bool validate_v8_record(const ChargeCycleConfigRecord_t *rec) {
    return validate_record_length(rec, CONFIG_V8_PAYLOAD_SIZE);
}

static bool validate_v7_record(const ChargeCycleConfigRecord_t *rec) {
    return validate_record_length(rec, CONFIG_V7_PAYLOAD_SIZE);
}

static bool validate_v6_record(const ChargeCycleConfigRecord_t *rec) {
    return validate_record_length(rec, CONFIG_V6_PAYLOAD_SIZE);
}

static bool validate_v5_record(const ChargeCycleConfigRecord_t *rec) {
    return validate_record_length(rec, CONFIG_V5_PAYLOAD_SIZE);
}

static bool is_buffer_blank(const uint8_t *buf, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        if (buf[i] != FLASH_BLANK_BYTE) return false;
    }
    return true;
}

/* ================= Storage Access Helpers ================= */

static uint32_t get_storage_capacity(void)
{
    if (BSP_SPIFlash_IsAvailable()) {
        return SPI_FLASH_SECTOR_SIZE; /* Sector 0 (4 KB) */
    }
    return BSP_FLASH_PAGE_SIZE; /* Internal Flash Page (2 KB) */
}

static bool storage_read_record(ChargeCycleConfigRecord_t *rec, uint32_t offset)
{
    if (BSP_SPIFlash_IsAvailable()) {
        return BSP_SPIFlash_Read(SPI_FLASH_CONFIG_BASE + offset, (uint8_t *)rec, CONFIG_RECORD_SIZE);
    }
    const uint8_t *src = internal_flash_at(BSP_CONFIG_FLASH_PAGE_ADDR + offset);
    memcpy(rec, src, CONFIG_RECORD_SIZE);
    return true;
}

static bool storage_is_blank(uint32_t offset)
{
    if (BSP_SPIFlash_IsAvailable()) {
        uint8_t buf[ALIGNED_RECORD_SIZE];
        if (!BSP_SPIFlash_Read(SPI_FLASH_CONFIG_BASE + offset, buf, ALIGNED_RECORD_SIZE)) {
            return false;
        }
        return is_buffer_blank(buf, ALIGNED_RECORD_SIZE);
    }
    const uint8_t *flash = internal_flash_at(BSP_CONFIG_FLASH_PAGE_ADDR + offset);
    return is_buffer_blank(flash, ALIGNED_RECORD_SIZE);
}

static int32_t find_blank_offset(void) {
    uint32_t capacity = get_storage_capacity();
    for (uint32_t offset = 0; offset <= (capacity - ALIGNED_RECORD_SIZE); offset += ALIGNED_RECORD_SIZE) {
        if (storage_is_blank(offset)) {
            return (int32_t)offset;
        }
    }
    return -1;
}

static bool write_single_record(uint32_t offset, const ChargeCycleConfig_t *config) {
    ChargeCycleConfigRecord_t record;
    uint8_t write_buf[ALIGNED_RECORD_SIZE];

    record.magic = CONFIG_MAGIC;
    record.version = CONFIG_RECORD_VERSION;
    record.length = sizeof(ChargeCycleConfig_t);
    record.crc32 = calc_crc32((const uint8_t *)config, sizeof(ChargeCycleConfig_t));
    record.payload = *config;

    memset(write_buf, FLASH_BLANK_BYTE, sizeof(write_buf));
    memcpy(write_buf, &record, CONFIG_RECORD_SIZE);

    if (BSP_SPIFlash_IsAvailable()) {
        if (!BSP_SPIFlash_Write(SPI_FLASH_CONFIG_BASE + offset, write_buf, ALIGNED_RECORD_SIZE)) {
            return false;
        }
    } else {
        if (!BSP_Flash_WriteBlock(BSP_CONFIG_FLASH_PAGE_ADDR + offset, write_buf, ALIGNED_RECORD_SIZE)) {
            return false;
        }
    }

    ChargeCycleConfigRecord_t verify;
    storage_read_record(&verify, offset);
    return validate_record(&verify);
}

static bool erase_storage(void)
{
    if (BSP_SPIFlash_IsAvailable()) {
        return BSP_SPIFlash_EraseSector4K(SPI_FLASH_CONFIG_BASE);
    }
    return BSP_Flash_ErasePage(BSP_CONFIG_FLASH_PAGE_ADDR);
}

/* ================= Load & Migration Logic ================= */

static bool scan_internal_flash_for_migration(ChargeCycleConfig_t *fast_cfg, ChargeCycleConfig_t *norm_cfg,
                                             uint8_t *active_mode_out)
{
    ChargeCycleConfigRecord_t record;
    int32_t latest_fast_offset = -1;
    int32_t latest_norm_offset = -1;
    int32_t last_valid_offset = -1;
    uint8_t last_valid_mode = DEFAULT_CHARGE_MODE;

    for (uint32_t offset = 0; offset <= (BSP_FLASH_PAGE_SIZE - ALIGNED_RECORD_SIZE); offset += ALIGNED_RECORD_SIZE) {
        const uint8_t *src = internal_flash_at(BSP_CONFIG_FLASH_PAGE_ADDR + offset);
        memcpy(&record, src, CONFIG_RECORD_SIZE);

        if (validate_record(&record)) {
            last_valid_offset = (int32_t)offset;
            if (record.payload.charge_mode == CHARGE_MODE_NORMAL) {
                latest_norm_offset = (int32_t)offset;
                last_valid_mode = CHARGE_MODE_NORMAL;
                *norm_cfg = record.payload;
            } else {
                latest_fast_offset = (int32_t)offset;
                last_valid_mode = CHARGE_MODE_FAST;
                *fast_cfg = record.payload;
            }
        } else if (validate_v8_record(&record)) {
            last_valid_offset = (int32_t)offset;
            if (record.payload.charge_mode == CHARGE_MODE_NORMAL) {
                latest_norm_offset = (int32_t)offset;
                last_valid_mode = CHARGE_MODE_NORMAL;
                memcpy(norm_cfg, &record.payload, CONFIG_V8_PAYLOAD_SIZE);
                norm_cfg->version = CHARGE_CYCLE_CONFIG_VERSION;
                norm_cfg->module_address = DEFAULT_MODULE_ADDRESS;
            } else {
                latest_fast_offset = (int32_t)offset;
                last_valid_mode = CHARGE_MODE_FAST;
                memcpy(fast_cfg, &record.payload, CONFIG_V8_PAYLOAD_SIZE);
                fast_cfg->version = CHARGE_CYCLE_CONFIG_VERSION;
                fast_cfg->module_address = DEFAULT_MODULE_ADDRESS;
            }
        } else if (validate_v7_record(&record)) {
            last_valid_offset = (int32_t)offset;
            if (record.payload.charge_mode == CHARGE_MODE_NORMAL) {
                latest_norm_offset = (int32_t)offset;
                last_valid_mode = CHARGE_MODE_NORMAL;
                memcpy(norm_cfg, &record.payload, CONFIG_V7_PAYLOAD_SIZE);
                norm_cfg->version = CHARGE_CYCLE_CONFIG_VERSION;
                norm_cfg->imax_a = 50.0f;
                norm_cfg->module_address = DEFAULT_MODULE_ADDRESS;
            } else {
                latest_fast_offset = (int32_t)offset;
                last_valid_mode = CHARGE_MODE_FAST;
                memcpy(fast_cfg, &record.payload, CONFIG_V7_PAYLOAD_SIZE);
                fast_cfg->version = CHARGE_CYCLE_CONFIG_VERSION;
                fast_cfg->imax_a = DEFAULT_IMAX_A;
                fast_cfg->module_address = DEFAULT_MODULE_ADDRESS;
            }
        } else if (validate_v6_record(&record) || validate_v5_record(&record)) {
            last_valid_offset = (int32_t)offset;
            latest_fast_offset = (int32_t)offset;
            last_valid_mode = CHARGE_MODE_FAST;
            ChargeCycleConfig_GetDefaults(fast_cfg);
            uint16_t copy_len = (record.length == CONFIG_V5_PAYLOAD_SIZE) ? CONFIG_V5_PAYLOAD_SIZE : CONFIG_V6_PAYLOAD_SIZE;
            memcpy(fast_cfg, &record.payload, copy_len);
            fast_cfg->version = CHARGE_CYCLE_CONFIG_VERSION;
            fast_cfg->charge_mode = CHARGE_MODE_FAST;
            if (copy_len == CONFIG_V5_PAYLOAD_SIZE) {
                fast_cfg->admin_pin = DEFAULT_ADMIN_PIN;
            }
        }
    }

    if (last_valid_offset < 0) {
        return false;
    }

    if (latest_fast_offset < 0) {
        ChargeCycleConfig_GetDefaults(fast_cfg);
        fast_cfg->charge_mode = CHARGE_MODE_FAST;
    }
    if (latest_norm_offset < 0) {
        ChargeCycleConfig_GetDefaults(norm_cfg);
        norm_cfg->charge_mode = CHARGE_MODE_NORMAL;
        norm_cfg->imax_c = 0.5f;
    }
    if (active_mode_out != NULL) {
        *active_mode_out = last_valid_mode;
    }
    return true;
}

static bool load_latest_configs(ChargeCycleConfig_t *fast_cfg, ChargeCycleConfig_t *norm_cfg,
                                uint8_t *active_mode_out, bool *migrated_out) {
    ChargeCycleConfigRecord_t record;
    int32_t latest_fast_offset = -1;
    int32_t latest_norm_offset = -1;
    int32_t last_valid_offset = -1;
    uint8_t last_valid_mode = DEFAULT_CHARGE_MODE;
    bool any_migrated = false;
    uint32_t capacity = get_storage_capacity();

    /* 1. Try reading from primary storage (SPI Flash if available, else Internal Flash) */
    for (uint32_t offset = 0; offset <= (capacity - ALIGNED_RECORD_SIZE); offset += ALIGNED_RECORD_SIZE) {
        if (!storage_read_record(&record, offset)) {
            continue;
        }
        if (validate_record(&record)) {
            last_valid_offset = (int32_t)offset;
            if (record.payload.charge_mode == CHARGE_MODE_NORMAL) {
                latest_norm_offset = (int32_t)offset;
                last_valid_mode = CHARGE_MODE_NORMAL;
                *norm_cfg = record.payload;
            } else {
                latest_fast_offset = (int32_t)offset;
                last_valid_mode = CHARGE_MODE_FAST;
                *fast_cfg = record.payload;
            }
        } else if (validate_v8_record(&record)) {
            last_valid_offset = (int32_t)offset;
            any_migrated = true;
            if (record.payload.charge_mode == CHARGE_MODE_NORMAL) {
                latest_norm_offset = (int32_t)offset;
                last_valid_mode = CHARGE_MODE_NORMAL;
                memcpy(norm_cfg, &record.payload, CONFIG_V8_PAYLOAD_SIZE);
                norm_cfg->version = CHARGE_CYCLE_CONFIG_VERSION;
                norm_cfg->module_address = DEFAULT_MODULE_ADDRESS;
            } else {
                latest_fast_offset = (int32_t)offset;
                last_valid_mode = CHARGE_MODE_FAST;
                memcpy(fast_cfg, &record.payload, CONFIG_V8_PAYLOAD_SIZE);
                fast_cfg->version = CHARGE_CYCLE_CONFIG_VERSION;
                fast_cfg->module_address = DEFAULT_MODULE_ADDRESS;
            }
        } else if (validate_v7_record(&record)) {
            last_valid_offset = (int32_t)offset;
            any_migrated = true;
            if (record.payload.charge_mode == CHARGE_MODE_NORMAL) {
                latest_norm_offset = (int32_t)offset;
                last_valid_mode = CHARGE_MODE_NORMAL;
                memcpy(norm_cfg, &record.payload, CONFIG_V7_PAYLOAD_SIZE);
                norm_cfg->version = CHARGE_CYCLE_CONFIG_VERSION;
                norm_cfg->imax_a = 50.0f;
                norm_cfg->module_address = DEFAULT_MODULE_ADDRESS;
            } else {
                latest_fast_offset = (int32_t)offset;
                last_valid_mode = CHARGE_MODE_FAST;
                memcpy(fast_cfg, &record.payload, CONFIG_V7_PAYLOAD_SIZE);
                fast_cfg->version = CHARGE_CYCLE_CONFIG_VERSION;
                fast_cfg->imax_a = DEFAULT_IMAX_A;
                fast_cfg->module_address = DEFAULT_MODULE_ADDRESS;
            }
        } else if (validate_v6_record(&record)) {
            last_valid_offset = (int32_t)offset;
            latest_fast_offset = (int32_t)offset;
            last_valid_mode = CHARGE_MODE_FAST;
            any_migrated = true;
            ChargeCycleConfig_GetDefaults(fast_cfg);
            memcpy(fast_cfg, &record.payload, CONFIG_V6_PAYLOAD_SIZE);
            fast_cfg->version = CHARGE_CYCLE_CONFIG_VERSION;
            fast_cfg->charge_mode = CHARGE_MODE_FAST;
            fast_cfg->delay_enabled = DEFAULT_DELAY_ENABLED;
            fast_cfg->delay_hours = DEFAULT_DELAY_HOURS;
            fast_cfg->delay_minutes = DEFAULT_DELAY_MINUTES;
        } else if (validate_v5_record(&record)) {
            last_valid_offset = (int32_t)offset;
            latest_fast_offset = (int32_t)offset;
            last_valid_mode = CHARGE_MODE_FAST;
            any_migrated = true;
            ChargeCycleConfig_GetDefaults(fast_cfg);
            memcpy(fast_cfg, &record.payload, CONFIG_V5_PAYLOAD_SIZE);
            fast_cfg->version = CHARGE_CYCLE_CONFIG_VERSION;
            fast_cfg->admin_pin = DEFAULT_ADMIN_PIN;
            fast_cfg->charge_mode = CHARGE_MODE_FAST;
            fast_cfg->delay_enabled = DEFAULT_DELAY_ENABLED;
            fast_cfg->delay_hours = DEFAULT_DELAY_HOURS;
            fast_cfg->delay_minutes = DEFAULT_DELAY_MINUTES;
        }
    }

    /* 2. If SPI Flash is available but was empty (first boot on SPI Flash), check Internal Flash to migrate */
    if (last_valid_offset < 0 && BSP_SPIFlash_IsAvailable()) {
        LOG("ChargeCycleStorage: External Flash empty, checking Internal Flash for migration...\r\n");
        if (scan_internal_flash_for_migration(fast_cfg, norm_cfg, &last_valid_mode)) {
            LOG("ChargeCycleStorage: [MIGRATE] Found valid config in Internal Flash! Migrating to External SPI Flash...\r\n");
            erase_storage();
            write_single_record(0, fast_cfg);
            write_single_record(ALIGNED_RECORD_SIZE, norm_cfg);
            last_valid_offset = 0;
            latest_fast_offset = 0;
            latest_norm_offset = ALIGNED_RECORD_SIZE;
            any_migrated = true;
        }
    }

    if (last_valid_offset < 0) {
        return false;
    }

    if (latest_fast_offset < 0) {
        ChargeCycleConfig_GetDefaults(fast_cfg);
        fast_cfg->charge_mode = CHARGE_MODE_FAST;
    }

    if (latest_norm_offset < 0) {
        ChargeCycleConfig_GetDefaults(norm_cfg);
        norm_cfg->charge_mode = CHARGE_MODE_NORMAL;
        norm_cfg->imax_c = 0.5f;
    }

    if (active_mode_out != NULL) {
        *active_mode_out = last_valid_mode;
    }
    if (migrated_out != NULL) {
        *migrated_out = any_migrated;
    }
    return true;
}

/* ================= Public Storage API ================= */

void ChargeCycleStorage_Init(void) {
    ChargeCycleConfig_t fast_cfg;
    ChargeCycleConfig_t norm_cfg;
    uint8_t active_mode = DEFAULT_CHARGE_MODE;
    bool migrated = false;

    if (load_latest_configs(&fast_cfg, &norm_cfg, &active_mode, &migrated)) {
        LOG("ChargeCycleStorage: Loaded configs from %s\r\n",
            BSP_SPIFlash_IsAvailable() ? "External SPI Flash" : "Internal Flash");
        ChargeCycleConfig_SetProfile(CHARGE_MODE_FAST, &fast_cfg);
        ChargeCycleConfig_SetProfile(CHARGE_MODE_NORMAL, &norm_cfg);
        /* Always boot into default NORMAL mode and NO DELAY (Option A retains delay_hours/minutes) */
        ChargeCycleConfig_ResetSessionDefaults();
        if (migrated) {
            LOG("ChargeCycleStorage: Migrated config to v%u on %s\r\n",
                (unsigned)CHARGE_CYCLE_CONFIG_VERSION,
                BSP_SPIFlash_IsAvailable() ? "External SPI Flash" : "Internal Flash");
            (void)ChargeCycleStorage_SaveProfile(CHARGE_MODE_FAST, &fast_cfg);
            (void)ChargeCycleStorage_SaveProfile(CHARGE_MODE_NORMAL, &norm_cfg);
        }
    } else {
        LOG("ChargeCycleStorage: Using default config on %s\r\n",
            BSP_SPIFlash_IsAvailable() ? "External SPI Flash" : "Internal Flash");
        ChargeCycleConfig_Init();
        ChargeCycleConfig_Get(&norm_cfg);
        ChargeCycleConfig_Set(&norm_cfg);
        /* Write default config immediately so flash is initialized */
        (void)ChargeCycleStorage_SaveProfile(CHARGE_MODE_NORMAL, &norm_cfg);
    }
}

bool ChargeCycleStorage_Load(ChargeCycleConfig_t *config) {
    ChargeCycleConfig_t fast_cfg;
    ChargeCycleConfig_t norm_cfg;
    uint8_t active_mode = DEFAULT_CHARGE_MODE;
    if (!load_latest_configs(&fast_cfg, &norm_cfg, &active_mode, NULL)) {
        return false;
    }
    if (config != NULL) {
        *config = (active_mode == CHARGE_MODE_NORMAL) ? norm_cfg : fast_cfg;
    }
    return true;
}

bool ChargeCycleStorage_SaveProfile(uint8_t mode, const ChargeCycleConfig_t *config) {
    if (config == NULL) {
        return false;
    }

    ChargeCycleConfig_t to_write = *config;
    to_write.charge_mode = (mode == CHARGE_MODE_NORMAL) ? CHARGE_MODE_NORMAL : CHARGE_MODE_FAST;

    int32_t write_offset = find_blank_offset();

    if (write_offset < 0) {
        LOG("ChargeCycleStorage: Sector/Page full, erasing and re-packing dual profiles...\r\n");
        if (!erase_storage()) {
            LOG("ChargeCycleStorage: Erase failed!\r\n");
            return false;
        }

        /* Preserve the other mode */
        uint8_t other_mode = (to_write.charge_mode == CHARGE_MODE_NORMAL) ? CHARGE_MODE_FAST : CHARGE_MODE_NORMAL;
        ChargeCycleConfig_t other_cfg;
        ChargeCycleConfig_GetProfile(other_mode, &other_cfg);

        if (!write_single_record(0, &other_cfg)) {
            LOG("ChargeCycleStorage: Write other profile failed!\r\n");
            return false;
        }

        if (!write_single_record(ALIGNED_RECORD_SIZE, &to_write)) {
            LOG("ChargeCycleStorage: Write target profile failed!\r\n");
            return false;
        }

        LOG("ChargeCycleStorage: Dual profiles re-packed successfully on %s\r\n",
            BSP_SPIFlash_IsAvailable() ? "External SPI Flash" : "Internal Flash");
        return true;
    }

    if (!write_single_record((uint32_t)write_offset, &to_write)) {
        LOG("ChargeCycleStorage: Write failed!\r\n");
        return false;
    }

    LOG("ChargeCycleStorage: Saved profile %u to %s (offset 0x%04lX)\r\n",
        (unsigned)to_write.charge_mode,
        BSP_SPIFlash_IsAvailable() ? "External SPI Flash" : "Internal Flash",
        (unsigned long)write_offset);
    return true;
}

bool ChargeCycleStorage_Save(const ChargeCycleConfig_t *config) {
    if (config == NULL) {
        return false;
    }
    return ChargeCycleStorage_SaveProfile(config->charge_mode, config);
}
