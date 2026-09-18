/**
 * @file    bsp_spi_flash.c
 * @brief   Board Support Package - External SPI NOR Flash Driver (SPI2)
 * @note    Implements JEDEC SPI Flash protocol over hspi2 with PB9 CS.
 */

#include "bsp_spi_flash.h"
#include "spi.h"
#include "main.h"
#include "debug_log.h"
#include <string.h>

/* SPI Flash Commands */
#define CMD_WRITE_ENABLE          0x06U
#define CMD_WRITE_DISABLE         0x04U
#define CMD_READ_STATUS_REG1      0x05U
#define CMD_READ_STATUS_REG2      0x35U
#define CMD_WRITE_STATUS_REG      0x01U
#define CMD_PAGE_PROGRAM          0x02U
#define CMD_SECTOR_ERASE_4K       0x20U
#define CMD_BLOCK_ERASE_32K       0x52U
#define CMD_BLOCK_ERASE_64K       0xD8U
#define CMD_CHIP_ERASE            0xC7U
#define CMD_READ_DATA             0x03U
#define CMD_FAST_READ             0x0BU
#define CMD_READ_JEDEC_ID         0x9FU
#define CMD_RELEASE_POWER_DOWN    0xABU

/* Status Register 1 Bits */
#define STATUS_REG1_BUSY          0x01U  /* Write in progress */
#define STATUS_REG1_WEL           0x02U  /* Write enable latch */

/* Communication Timeouts */
#define SPI_TIMEOUT_MS            100U
#define ERASE_TIMEOUT_MS          3000U
#define CHIP_ERASE_TIMEOUT_MS     60000U

/* Internal State */
static bool s_flash_available = false;
static uint32_t s_jedec_id = 0U;
static uint32_t s_capacity_bytes = 0U;

/* Helper: CS Pin Control */
static inline void cs_low(void)
{
    HAL_GPIO_WritePin(MCU_PB9_SPI2_CS_GPIO_Port, MCU_PB9_SPI2_CS_Pin, GPIO_PIN_RESET);
}

static inline void cs_high(void)
{
    HAL_GPIO_WritePin(MCU_PB9_SPI2_CS_GPIO_Port, MCU_PB9_SPI2_CS_Pin, GPIO_PIN_SET);
}

/* Helper: SPI Transmit/Receive */
static bool spi_transmit(const uint8_t *data, uint16_t len)
{
    return HAL_SPI_Transmit(&hspi2, (uint8_t *)data, len, SPI_TIMEOUT_MS) == HAL_OK;
}

static bool spi_receive(uint8_t *buf, uint16_t len)
{
    return HAL_SPI_Receive(&hspi2, buf, len, SPI_TIMEOUT_MS) == HAL_OK;
}

/* Helper: Read Status Register 1 */
static uint8_t read_status_reg1(void)
{
    uint8_t cmd = CMD_READ_STATUS_REG1;
    uint8_t status = 0xFFU;

    cs_low();
    if (spi_transmit(&cmd, 1U)) {
        spi_receive(&status, 1U);
    }
    cs_high();
    return status;
}

/* Helper: Wait for Write/Erase completion */
static bool wait_not_busy(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    while ((read_status_reg1() & STATUS_REG1_BUSY) != 0U) {
        if ((HAL_GetTick() - start) > timeout_ms) {
            LOG("SPI_Flash: Timeout waiting BUSY clear\r\n");
            return false;
        }
    }
    return true;
}

/* Helper: Send Write Enable */
static bool write_enable(void)
{
    uint8_t cmd = CMD_WRITE_ENABLE;
    cs_low();
    bool ok = spi_transmit(&cmd, 1U);
    cs_high();

    if (!ok) return false;
    return (read_status_reg1() & STATUS_REG1_WEL) != 0U;
}

/* ================= Public Functions ================= */

bool BSP_SPIFlash_Init(void)
{
    cs_high();
    HAL_Delay(5); /* Power-on settle time */

    /* Release from Deep Power-Down (just in case) */
    uint8_t cmd_wake = CMD_RELEASE_POWER_DOWN;
    cs_low();
    spi_transmit(&cmd_wake, 1U);
    cs_high();
    HAL_Delay(1);

    /* Read JEDEC ID (0x9F) */
    uint8_t cmd_id = CMD_READ_JEDEC_ID;
    uint8_t id_bytes[3] = {0};

    cs_low();
    if (spi_transmit(&cmd_id, 1U)) {
        spi_receive(id_bytes, 3U);
    }
    cs_high();

    s_jedec_id = ((uint32_t)id_bytes[0] << 16) | ((uint32_t)id_bytes[1] << 8) | id_bytes[2];

    /* Validate JEDEC ID (Must not be 0x000000 or 0xFFFFFF) */
    if (s_jedec_id == 0x000000U || s_jedec_id == 0xFFFFFFU) {
        LOG("SPI_Flash: No chip detected (JEDEC=0x%06lX)\r\n", (unsigned long)s_jedec_id);
        s_flash_available = false;
        s_capacity_bytes = 0U;
        return false;
    }

    /* Decode Capacity (2^N bytes, where N is id_bytes[2]) */
    uint8_t cap_code = id_bytes[2];
    if (cap_code >= 0x14U && cap_code <= 0x19U) {
        s_capacity_bytes = 1UL << cap_code; /* e.g. 0x15 -> 2^21 = 2MB, 0x16 -> 4MB, 0x17 -> 8MB */
    } else {
        s_capacity_bytes = 4UL * 1024UL * 1024UL; /* Default fallback: 4MB */
    }

    s_flash_available = true;
    LOG("SPI_Flash: Detected JEDEC=0x%06lX (Mfr=0x%02X, Cap=%lu KB)\r\n",
        (unsigned long)s_jedec_id, (unsigned)id_bytes[0], (unsigned long)(s_capacity_bytes / 1024UL));

    return true;
}

bool BSP_SPIFlash_IsAvailable(void)
{
    return s_flash_available;
}

uint32_t BSP_SPIFlash_GetCapacity(void)
{
    return s_capacity_bytes;
}

uint32_t BSP_SPIFlash_GetJedecId(void)
{
    return s_jedec_id;
}

bool BSP_SPIFlash_Read(uint32_t address, uint8_t *buf, uint32_t len)
{
    if (!s_flash_available || buf == NULL || len == 0U) {
        return false;
    }
    if ((address + len) > s_capacity_bytes) {
        return false;
    }

    if (!wait_not_busy(SPI_TIMEOUT_MS)) {
        return false;
    }

    uint8_t cmd[4];
    cmd[0] = CMD_READ_DATA;
    cmd[1] = (uint8_t)((address >> 16) & 0xFFU);
    cmd[2] = (uint8_t)((address >> 8) & 0xFFU);
    cmd[3] = (uint8_t)(address & 0xFFU);

    cs_low();
    bool ok = spi_transmit(cmd, 4U) && spi_receive(buf, (uint16_t)len);
    cs_high();

    return ok;
}

/* Internal helper: write up to one single page (max 256 bytes within page boundary) */
static bool write_page(uint32_t address, const uint8_t *data, uint16_t len)
{
    if (!write_enable()) {
        return false;
    }

    uint8_t cmd[4];
    cmd[0] = CMD_PAGE_PROGRAM;
    cmd[1] = (uint8_t)((address >> 16) & 0xFFU);
    cmd[2] = (uint8_t)((address >> 8) & 0xFFU);
    cmd[3] = (uint8_t)(address & 0xFFU);

    cs_low();
    bool ok = spi_transmit(cmd, 4U) && spi_transmit(data, len);
    cs_high();

    if (!ok) return false;
    return wait_not_busy(SPI_TIMEOUT_MS);
}

bool BSP_SPIFlash_Write(uint32_t address, const uint8_t *data, uint32_t len)
{
    if (!s_flash_available || data == NULL || len == 0U) {
        return false;
    }
    if ((address + len) > s_capacity_bytes) {
        return false;
    }

    uint32_t curr_addr = address;
    const uint8_t *curr_ptr = data;
    uint32_t remaining = len;

    while (remaining > 0U) {
        /* Bytes until the end of current 256-byte page */
        uint32_t page_offset = curr_addr % SPI_FLASH_PAGE_SIZE;
        uint32_t chunk = SPI_FLASH_PAGE_SIZE - page_offset;
        if (chunk > remaining) {
            chunk = remaining;
        }

        if (!write_page(curr_addr, curr_ptr, (uint16_t)chunk)) {
            LOG("SPI_Flash: Write failed at 0x%08lX\r\n", (unsigned long)curr_addr);
            return false;
        }

        curr_addr += chunk;
        curr_ptr += chunk;
        remaining -= chunk;
    }

    return true;
}

bool BSP_SPIFlash_EraseSector4K(uint32_t address)
{
    if (!s_flash_available || address >= s_capacity_bytes) {
        return false;
    }
    if (!wait_not_busy(SPI_TIMEOUT_MS)) {
        return false;
    }
    if (!write_enable()) {
        return false;
    }

    uint8_t cmd[4];
    cmd[0] = CMD_SECTOR_ERASE_4K;
    cmd[1] = (uint8_t)((address >> 16) & 0xFFU);
    cmd[2] = (uint8_t)((address >> 8) & 0xFFU);
    cmd[3] = (uint8_t)(address & 0xFFU);

    cs_low();
    bool ok = spi_transmit(cmd, 4U);
    cs_high();

    if (!ok) return false;
    return wait_not_busy(ERASE_TIMEOUT_MS);
}

bool BSP_SPIFlash_EraseBlock32K(uint32_t address)
{
    if (!s_flash_available || address >= s_capacity_bytes) {
        return false;
    }
    if (!wait_not_busy(SPI_TIMEOUT_MS)) {
        return false;
    }
    if (!write_enable()) {
        return false;
    }

    uint8_t cmd[4];
    cmd[0] = CMD_BLOCK_ERASE_32K;
    cmd[1] = (uint8_t)((address >> 16) & 0xFFU);
    cmd[2] = (uint8_t)((address >> 8) & 0xFFU);
    cmd[3] = (uint8_t)(address & 0xFFU);

    cs_low();
    bool ok = spi_transmit(cmd, 4U);
    cs_high();

    if (!ok) return false;
    return wait_not_busy(ERASE_TIMEOUT_MS * 2U);
}

bool BSP_SPIFlash_EraseBlock64K(uint32_t address)
{
    if (!s_flash_available || address >= s_capacity_bytes) {
        return false;
    }
    if (!wait_not_busy(SPI_TIMEOUT_MS)) {
        return false;
    }
    if (!write_enable()) {
        return false;
    }

    uint8_t cmd[4];
    cmd[0] = CMD_BLOCK_ERASE_64K;
    cmd[1] = (uint8_t)((address >> 16) & 0xFFU);
    cmd[2] = (uint8_t)((address >> 8) & 0xFFU);
    cmd[3] = (uint8_t)(address & 0xFFU);

    cs_low();
    bool ok = spi_transmit(cmd, 4U);
    cs_high();

    if (!ok) return false;
    return wait_not_busy(ERASE_TIMEOUT_MS * 4U);
}

bool BSP_SPIFlash_ChipErase(void)
{
    if (!s_flash_available) {
        return false;
    }
    if (!wait_not_busy(SPI_TIMEOUT_MS)) {
        return false;
    }
    if (!write_enable()) {
        return false;
    }

    uint8_t cmd = CMD_CHIP_ERASE;
    cs_low();
    bool ok = spi_transmit(&cmd, 1U);
    cs_high();

    if (!ok) return false;
    return wait_not_busy(CHIP_ERASE_TIMEOUT_MS);
}
