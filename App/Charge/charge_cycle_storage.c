/**
 * @file charge_cycle_storage.c
 * @brief Flash persistence for ChargeCycleConfig_t using BSP_Flash
 */

#include "charge_cycle_storage.h"
#include "debug_log.h"
#include "bsp_flash.h"
#include <string.h>
#include <stddef.h>

#define CONFIG_MAGIC          0x43434647U
#define CONFIG_RECORD_VERSION 1U
#define FLASH_BLANK_BYTE      0xFFU
#define CONFIG_V5_PAYLOAD_SIZE 239U

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
#define ALIGNED_RECORD_SIZE    ((CONFIG_RECORD_SIZE + 7) & ~7) // Align to 8 bytes for G0 double-word

_Static_assert(offsetof(ChargeCycleConfig_t, admin_pin) == CONFIG_V5_PAYLOAD_SIZE,
               "v6 must append admin_pin after the v5 payload");

static const uint8_t *config_flash_at(uint32_t address)
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

static bool validate_v5_record(const ChargeCycleConfigRecord_t *rec) {
    return validate_record_length(rec, CONFIG_V5_PAYLOAD_SIZE);
}

static bool is_flash_blank(const uint8_t *addr, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        if (addr[i] != FLASH_BLANK_BYTE) return false;
    }
    return true;
}

static int32_t find_blank_offset(void) {
    const uint8_t *flash = config_flash_at(BSP_CONFIG_FLASH_PAGE_ADDR);
    
    for (uint32_t offset = 0; offset <= (BSP_FLASH_PAGE_SIZE - ALIGNED_RECORD_SIZE); offset += ALIGNED_RECORD_SIZE) {
        if (is_flash_blank(flash + offset, ALIGNED_RECORD_SIZE)) {
            return (int32_t)offset;
        }
    }
    return -1;
}

static void read_record_at(ChargeCycleConfigRecord_t *rec, uint32_t offset) {
    const uint8_t *src = config_flash_at(BSP_CONFIG_FLASH_PAGE_ADDR + offset);
    memcpy(rec, src, CONFIG_RECORD_SIZE);
}

static bool load_latest_config(ChargeCycleConfig_t *config, uint32_t *latest_offset_out,
                               bool *migrated_v5_out) {
    ChargeCycleConfigRecord_t record;
    int32_t latest_valid_offset = -1;
    bool latest_is_v5 = false;

    for (uint32_t offset = 0; offset <= (BSP_FLASH_PAGE_SIZE - ALIGNED_RECORD_SIZE); offset += ALIGNED_RECORD_SIZE) {
        read_record_at(&record, offset);
        if (validate_record(&record)) {
            latest_valid_offset = (int32_t)offset;
            latest_is_v5 = false;
        } else if (validate_v5_record(&record)) {
            latest_valid_offset = (int32_t)offset;
            latest_is_v5 = true;
        }
    }

    if (latest_valid_offset < 0) return false;

    read_record_at(&record, (uint32_t)latest_valid_offset);
    if (latest_is_v5) {
        /* v6 only appends admin_pin. Start from defaults, copy the exact v5
         * prefix, then install the default PIN before normal validation. */
        ChargeCycleConfig_GetDefaults(config);
        memcpy(config, &record.payload, CONFIG_V5_PAYLOAD_SIZE);
        config->version = CHARGE_CYCLE_CONFIG_VERSION;
        config->admin_pin = DEFAULT_ADMIN_PIN;
    } else {
        *config = record.payload;
    }

    if (latest_offset_out != NULL) {
        *latest_offset_out = (uint32_t)latest_valid_offset;
    }
    if (migrated_v5_out != NULL) {
        *migrated_v5_out = latest_is_v5;
    }
    return true;
}

void ChargeCycleStorage_Init(void) {
    ChargeCycleConfig_t config;
    uint32_t offset = 0;
    bool migrated_v5 = false;

    if (load_latest_config(&config, &offset, &migrated_v5)) {
        LOG("ChargeCycleStorage: Loaded from flash offset %u\r\n", (unsigned)offset);
        ChargeCycleConfig_Set(&config);
        if (migrated_v5) {
            LOG("ChargeCycleStorage: Migrated v5 config to v6\r\n");
            if (!ChargeCycleStorage_Save(&config)) {
                LOG("ChargeCycleStorage: v6 migration save failed\r\n");
            }
        }
    } else {
        LOG("ChargeCycleStorage: Using default config\r\n");
        /* BUGFIX: still run the RAM defaults through ChargeCycleConfig_Set()
         * so its module_type -> driver_id mapping and module
         * auto-registration side effects apply on a genuinely blank-flash
         * first boot too, not just the "loaded a real record" path above.
         * Previously this branch left the driver unselected
         * (CHG_LIB_GetActiveDriverId() == CHG_LIB_DRV_NONE) and zero
         * modules registered: ChargeCycleConfig_GetDefaults() sets
         * module_type = CHARGE_MODULE_TYPE_EVR_10KW_100A_100V, and only
         * ChargeCycleConfig_Set()'s own switch statement knows that maps
         * to CHG_LIB_DRV_TONHE -- App_Init() (app_main.c) used to
         * re-implement a narrower module_type range check that silently
         * excluded exactly that value, so it never ran. Calling Set() here
         * makes this the single source of truth for that mapping; the
         * app_main.c workaround has been removed. */
        ChargeCycleConfig_Get(&config);
        ChargeCycleConfig_Set(&config);
    }
}

bool ChargeCycleStorage_Load(ChargeCycleConfig_t *config) {
    uint32_t offset = 0;
    return load_latest_config(config, &offset, NULL);
}

bool ChargeCycleStorage_Save(const ChargeCycleConfig_t *config) {
    ChargeCycleConfigRecord_t record;
    /* BUG-02 fix: ChargeCycleConfigRecord_t is CONFIG_RECORD_SIZE (251B:
     * 12B header + 239B payload) packed bytes, but flash writes must be
     * ALIGNED_RECORD_SIZE (256B, rounded up to the G0 double-word boundary).
     * Writing directly from &record for ALIGNED_RECORD_SIZE bytes reads past
     * the end of the local `record` variable (stack OOB read, UB) and burns
     * whatever
     * garbage happened to be there into flash instead of well-defined
     * padding. Stage the write in a correctly-sized, blank-initialized
     * buffer instead. */
    uint8_t write_buf[ALIGNED_RECORD_SIZE];
    int32_t write_offset;

    record.magic = CONFIG_MAGIC;
    record.version = CONFIG_RECORD_VERSION;
    record.length = sizeof(ChargeCycleConfig_t);
    record.crc32 = calc_crc32((const uint8_t *)config, sizeof(ChargeCycleConfig_t));
    record.payload = *config;

    memset(write_buf, FLASH_BLANK_BYTE, sizeof(write_buf));
    memcpy(write_buf, &record, CONFIG_RECORD_SIZE);

    write_offset = find_blank_offset();

    if (write_offset < 0) {
        LOG("ChargeCycleStorage: Page full, erasing...\r\n");
        if (!BSP_Flash_ErasePage(BSP_CONFIG_FLASH_PAGE_ADDR)) {
            LOG("ChargeCycleStorage: Erase failed!\r\n");
            return false;
        }
        write_offset = 0;
    }

    if (!BSP_Flash_WriteBlock(BSP_CONFIG_FLASH_PAGE_ADDR + write_offset, write_buf, ALIGNED_RECORD_SIZE)) {
        LOG("ChargeCycleStorage: Write failed!\r\n");
        return false;
    }

    ChargeCycleConfigRecord_t verify;
    read_record_at(&verify, (uint32_t)write_offset);
    if (!validate_record(&verify)) {
        LOG("ChargeCycleStorage: Verification failed!\r\n");
        return false;
    }

    LOG("ChargeCycleStorage: Saved successfully\r\n");
    return true;
}
