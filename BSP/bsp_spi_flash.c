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
static bool s_use_bitbang = false;
static volatile uint32_t s_jedec_id = 0U;
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

/* Bitbang helpers (PB6: MOSI -> Flash Pin 5 SI, PB7: MISO <- Flash Pin 2 SO, PB8: SCK) */
static inline void bb_delay(void)
{
    for (volatile int d = 0; d < 6; d++) {}
}

static void init_bitbang_gpio(void)
{
    GPIO_InitTypeDef gpio = {0};
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_9, GPIO_PIN_SET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_8, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_6, GPIO_PIN_SET);

    gpio.Pin = GPIO_PIN_8 | GPIO_PIN_9 | GPIO_PIN_6;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(GPIOB, &gpio);

    gpio.Pin = GPIO_PIN_7;
    gpio.Mode = GPIO_MODE_INPUT;
    gpio.Pull = GPIO_PULLUP;
    HAL_GPIO_Init(GPIOB, &gpio);
}

static bool bb_transmit(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0U; i < len; ++i) {
        uint8_t b = data[i];
        for (int bit = 7; bit >= 0; --bit) {
            if (b & (1 << bit)) {
                GPIOB->BSRR = GPIO_PIN_6;
            } else {
                GPIOB->BRR = GPIO_PIN_6;
            }
            bb_delay();
            GPIOB->BSRR = GPIO_PIN_8;
            bb_delay();
            GPIOB->BRR = GPIO_PIN_8;
            bb_delay();
        }
    }
    return true;
}

static bool bb_receive(uint8_t *buf, uint16_t len)
{
    for (uint16_t i = 0U; i < len; ++i) {
        uint8_t b = 0U;
        for (int bit = 7; bit >= 0; --bit) {
            GPIOB->BSRR = GPIO_PIN_8;
            bb_delay();
            if (GPIOB->IDR & GPIO_PIN_7) {
                b |= (1U << bit);
            }
            GPIOB->BRR = GPIO_PIN_8;
            bb_delay();
        }
        buf[i] = b;
    }
    return true;
}

/* Helper: SPI Transmit/Receive */
static bool spi_transmit(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0U) return false;

    if (s_use_bitbang) {
        return bb_transmit(data, len);
    }

    while (__HAL_SPI_GET_FLAG(&hspi2, SPI_FLAG_RXNE)) {
        __IO uint8_t dummy = *((__IO uint8_t *)&hspi2.Instance->DR);
        (void)dummy;
    }
    __HAL_SPI_CLEAR_OVRFLAG(&hspi2);

    if (HAL_SPI_Transmit(&hspi2, (uint8_t *)data, len, SPI_TIMEOUT_MS) != HAL_OK) {
        return false;
    }
    while (__HAL_SPI_GET_FLAG(&hspi2, SPI_FLAG_BSY)) {}

    while (__HAL_SPI_GET_FLAG(&hspi2, SPI_FLAG_RXNE)) {
        __IO uint8_t dummy = *((__IO uint8_t *)&hspi2.Instance->DR);
        (void)dummy;
    }
    __HAL_SPI_CLEAR_OVRFLAG(&hspi2);
    return true;
}

static bool spi_receive(uint8_t *buf, uint16_t len)
{
    if (buf == NULL || len == 0U) return false;

    if (s_use_bitbang) {
        return bb_receive(buf, len);
    }

    while (__HAL_SPI_GET_FLAG(&hspi2, SPI_FLAG_RXNE)) {
        __IO uint8_t dummy = *((__IO uint8_t *)&hspi2.Instance->DR);
        (void)dummy;
    }
    __HAL_SPI_CLEAR_OVRFLAG(&hspi2);

    memset(buf, 0xFF, len);
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

static bool read_jedec(uint8_t id_bytes[3])
{
    /* Release from Deep Power-Down */
    uint8_t cmd_wake = CMD_RELEASE_POWER_DOWN;
    cs_low();
    spi_transmit(&cmd_wake, 1U);
    cs_high();
    HAL_Delay(2);

    for (uint8_t retry = 0U; retry < 3U; ++retry) {
        uint8_t cmd_id = CMD_READ_JEDEC_ID;
        memset(id_bytes, 0, 3U);
        cs_low();
        if (spi_transmit(&cmd_id, 1U)) {
            spi_receive(id_bytes, 3U);
        }
        cs_high();

        s_jedec_id = ((uint32_t)id_bytes[0] << 16) | ((uint32_t)id_bytes[1] << 8) | id_bytes[2];
        if (s_jedec_id != 0x000000U && s_jedec_id != 0xFFFFFFU) {
            return true;
        }
        HAL_Delay(5);
    }
    return false;
}

bool BSP_SPIFlash_Init(void)
{
    cs_high();
    HAL_Delay(10); /* Power-on settle time */

    uint8_t id_bytes[3] = {0};

    /* 1. Try Hardware SPI2 first */
    s_use_bitbang = false;
    if (!read_jedec(id_bytes)) {
        /* 2. Fallback: Hardware PCB has PB6/PB7 swapped (MOSI->Pin5, MISO<-Pin2).
         * Switch to bitbang mode. */
        init_bitbang_gpio();
        s_use_bitbang = true;
        if (!read_jedec(id_bytes)) {
            LOG("SPI_Flash: No chip detected (JEDEC=0x%06lX)\r\n", (unsigned long)s_jedec_id);
            s_flash_available = false;
            s_capacity_bytes = 0U;
            return false;
        }
    }

    /* Decode Capacity (2^N bytes, where N is id_bytes[2]) */
    uint8_t cap_code = id_bytes[2];
    if (cap_code >= 0x14U && cap_code <= 0x19U) {
        s_capacity_bytes = 1UL << cap_code; /* e.g. 0x15 -> 2MB, 0x16 -> 4MB, 0x17 -> 8MB */
    } else {
        s_capacity_bytes = 8UL * 1024UL * 1024UL; /* Default fallback: 8MB */
    }

    s_flash_available = true;
    LOG("SPI_Flash: Detected JEDEC=0x%06lX (Mfr=0x%02X, Cap=%lu KB, mode=%s)\r\n",
        (unsigned long)s_jedec_id, (unsigned)id_bytes[0], (unsigned long)(s_capacity_bytes / 1024UL),
        s_use_bitbang ? "bitbang" : "hw_spi");

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

bool BSP_SPIFlash_SelfTest(uint32_t *out_jedec, uint32_t *out_cap_kb)
{
    if (!s_flash_available) {
        (void)BSP_SPIFlash_Init();
    }
    if (out_jedec != NULL) {
        *out_jedec = s_jedec_id;
    }
    if (out_cap_kb != NULL) {
        *out_cap_kb = s_capacity_bytes / 1024UL;
    }
    if (!s_flash_available) {
        return false;
    }

    /* Perform write-read-verify on test sector (OTA Staging base 0x00020000) */
    const uint32_t test_addr = 0x00020000U;
    if (!BSP_SPIFlash_EraseSector4K(test_addr)) {
        return false;
    }

    /* Verify erased to 0xFF */
    uint8_t check_buf[16];
    if (!BSP_SPIFlash_Read(test_addr, check_buf, sizeof(check_buf))) {
        return false;
    }
    for (uint32_t i = 0U; i < sizeof(check_buf); ++i) {
        if (check_buf[i] != 0xFFU) {
            return false;
        }
    }

    /* Write test pattern */
    static const uint8_t test_pattern[16] = {
        0xA5, 0x5A, 0x12, 0x34, 0x56, 0x78, 0x9A, 0xBC,
        0xDE, 0xF0, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66
    };
    if (!BSP_SPIFlash_Write(test_addr, test_pattern, sizeof(test_pattern))) {
        return false;
    }

    /* Read back and verify */
    memset(check_buf, 0, sizeof(check_buf));
    if (!BSP_SPIFlash_Read(test_addr, check_buf, sizeof(check_buf))) {
        return false;
    }
    if (memcmp(check_buf, test_pattern, sizeof(test_pattern)) != 0) {
        return false;
    }

    /* Erase sector clean after test */
    (void)BSP_SPIFlash_EraseSector4K(test_addr);

    return true;
}
