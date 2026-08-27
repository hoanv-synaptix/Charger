/**
 * @file charge_cycle_storage.c
 * @brief Flash persistence for ChargeCycleConfig_t using BSP_Flash
 */

#include "charge_cycle_storage.h"
#include "debug_log.h"
#include "bsp_flash.h"
#include <string.h>

#define CONFIG_MAGIC          0x43434647U
#define CONFIG_RECORD_VERSION 1U
#define FLASH_BLANK_BYTE      0xFFU

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint16_t version;
    uint16_t length;
    uint32_t crc32;
    ChargeCycleConfig_t payload;
} ChargeCycleConfigRecord_t;

#define CONFIG_RECORD_SIZE     (sizeof(ChargeCycleConfigRecord_t))
#define ALIGNED_RECORD_SIZE    ((CONFIG_RECORD_SIZE + 7) & ~7) // Align to 8 bytes for G0 double-word

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

static bool validate_record(const ChargeCycleConfigRecord_t *rec) {
    if (rec->magic != CONFIG_MAGIC) return false;
    if (rec->version != CONFIG_RECORD_VERSION) return false;
    if (rec->length != sizeof(ChargeCycleConfig_t)) return false;
    
    uint32_t calc_crc = calc_crc32((const uint8_t *)&rec->payload, sizeof(ChargeCycleConfig_t));
    return rec->crc32 == calc_crc;
}

static bool is_flash_blank(const uint8_t *addr, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        if (addr[i] != FLASH_BLANK_BYTE) return false;
    }
    return true;
}

static int32_t find_blank_offset(void) {
    const uint8_t *flash = (const uint8_t *)BSP_CONFIG_FLASH_PAGE_ADDR;
    
    for (uint32_t offset = 0; offset <= (BSP_FLASH_PAGE_SIZE - ALIGNED_RECORD_SIZE); offset += ALIGNED_RECORD_SIZE) {
        if (is_flash_blank(flash + offset, ALIGNED_RECORD_SIZE)) {
            return (int32_t)offset;
        }
    }
    return -1;
}

static void read_record_at(ChargeCycleConfigRecord_t *rec, uint32_t offset) {
    const uint8_t *src = (const uint8_t *)(BSP_CONFIG_FLASH_PAGE_ADDR + offset);
    memcpy(rec, src, CONFIG_RECORD_SIZE);
}

static bool load_latest_config(ChargeCycleConfig_t *config, uint32_t *latest_offset_out) {
    ChargeCycleConfigRecord_t record;
    int32_t latest_valid_offset = -1;

    for (uint32_t offset = 0; offset <= (BSP_FLASH_PAGE_SIZE - ALIGNED_RECORD_SIZE); offset += ALIGNED_RECORD_SIZE) {
        read_record_at(&record, offset);
        if (validate_record(&record)) {
            latest_valid_offset = (int32_t)offset;
        }
    }

    if (latest_valid_offset < 0) return false;

    read_record_at(&record, (uint32_t)latest_valid_offset);
    *config = record.payload;

    if (latest_offset_out != NULL) {
        *latest_offset_out = (uint32_t)latest_valid_offset;
    }
    return true;
}

void ChargeCycleStorage_Init(void) {
    ChargeCycleConfig_t config;
    uint32_t offset = 0;

    if (load_latest_config(&config, &offset)) {
        LOG("ChargeCycleStorage: Loaded from flash offset %u\r\n", (unsigned)offset);
        ChargeCycleConfig_Set(&config);
    } else {
        LOG("ChargeCycleStorage: Using default config\r\n");
    }
}

bool ChargeCycleStorage_Load(ChargeCycleConfig_t *config) {
    uint32_t offset = 0;
    return load_latest_config(config, &offset);
}

bool ChargeCycleStorage_Save(const ChargeCycleConfig_t *config) {
    ChargeCycleConfigRecord_t record;
    /* BUG-02 fix: ChargeCycleConfigRecord_t is CONFIG_RECORD_SIZE (219B)
     * packed bytes, but flash writes must be ALIGNED_RECORD_SIZE (224B,
     * rounded up to the G0 double-word boundary). Writing directly from
     * &record for ALIGNED_RECORD_SIZE bytes reads 5 bytes past the end of
     * the local `record` variable (stack OOB read, UB) and burns whatever
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

