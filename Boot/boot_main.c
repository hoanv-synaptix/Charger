/**
 * @file boot_main.c
 * @brief STM32G0B1 8 KiB OTA bootloader with external staging/backup.
 *
 * The bootloader deliberately verifies CRC32 only. SHA-256 is performed by
 * the application before the descriptor becomes VERIFIED; keeping the
 * bootloader small preserves the 120 KiB application region.
 */

#include "stm32g0xx_hal.h"
#include "../App/OTA/ota_types.h"
#include "../BSP/bsp_spi_flash.h"
#include <string.h>

#define APP_START_ADDR                  0x08002000U
#define APP_START_PAGE                  4U
#define STM32G0_FLASH_PAGE_SIZE         2048U
#define SPI_FLASH_BUSY_TIMEOUT_MS       10000U
#define SPI_FLASH_WRITE_TIMEOUT_MS      1000U
#define OTA_BOOT_ATTEMPT_LIMIT          3U
#define OTA_BACKUP_VALID_MAGIC          0xBACC0FFEU

/* Hardware Pins */
#define SPI2_CS_PORT                    GPIOB
#define SPI2_CS_PIN                     GPIO_PIN_9
#define LED_PORT                        GPIOC
#define LED_PIN                         GPIO_PIN_6

static SPI_HandleTypeDef hspi2;
static bool s_spi_ok;

void SysTick_Handler(void)
{
    HAL_IncTick();
}

void HardFault_Handler(void)
{
    NVIC_SystemReset();
}

static void SystemClock_Config(void)
{
    RCC_OscInitTypeDef osc = {0};
    RCC_ClkInitTypeDef clk = {0};
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    osc.HSIState = RCC_HSI_ON;
    osc.HSIDiv = RCC_HSI_DIV1;
    osc.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    (void)HAL_RCC_OscConfig(&osc);
    clk.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV1;
    (void)HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_0);
}

static void GPIO_Init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    GPIO_InitTypeDef gpio = {0};
    HAL_GPIO_WritePin(SPI2_CS_PORT, SPI2_CS_PIN, GPIO_PIN_SET);
    gpio.Pin = SPI2_CS_PIN;
    gpio.Mode = GPIO_MODE_OUTPUT_PP;
    gpio.Pull = GPIO_NOPULL;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(SPI2_CS_PORT, &gpio);

    HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_RESET);
    gpio.Pin = LED_PIN;
    gpio.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LED_PORT, &gpio);

    gpio.Pin = GPIO_PIN_8 | GPIO_PIN_7;
    gpio.Mode = GPIO_MODE_AF_PP;
    gpio.Speed = GPIO_SPEED_FREQ_HIGH;
    gpio.Alternate = GPIO_AF1_SPI2;
    HAL_GPIO_Init(GPIOB, &gpio);
    gpio.Pin = GPIO_PIN_6;
    gpio.Alternate = GPIO_AF4_SPI2;
    HAL_GPIO_Init(GPIOB, &gpio);
}

static void SPI2_Init(void)
{
    __HAL_RCC_SPI2_CLK_ENABLE();
    hspi2.Instance = SPI2;
    hspi2.Init.Mode = SPI_MODE_MASTER;
    hspi2.Init.Direction = SPI_DIRECTION_2LINES;
    hspi2.Init.DataSize = SPI_DATASIZE_8BIT;
    hspi2.Init.CLKPolarity = SPI_POLARITY_LOW;
    hspi2.Init.CLKPhase = SPI_PHASE_1EDGE;
    hspi2.Init.NSS = SPI_NSS_SOFT;
    hspi2.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2;
    hspi2.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi2.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi2.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    hspi2.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
    (void)HAL_SPI_Init(&hspi2);
}

static inline void cs_low(void) { HAL_GPIO_WritePin(SPI2_CS_PORT, SPI2_CS_PIN, GPIO_PIN_RESET); }
static inline void cs_high(void) { HAL_GPIO_WritePin(SPI2_CS_PORT, SPI2_CS_PIN, GPIO_PIN_SET); }

static uint8_t spi_transfer(uint8_t byte)
{
    if (!s_spi_ok) return 0xFFU;
    uint8_t rx = 0xFFU;
    if (HAL_SPI_TransmitReceive(&hspi2, &byte, &rx, 1U, 10U) != HAL_OK) {
        s_spi_ok = false;
    }
    return rx;
}

static bool spi_flash_wait_busy(uint32_t timeout_ms)
{
    uint32_t start = HAL_GetTick();
    do {
        uint8_t status;
        s_spi_ok = true;
        cs_low();
        (void)spi_transfer(0x05U);
        status = spi_transfer(0xFFU);
        cs_high();
        if (!s_spi_ok) return false;
        if ((status & 0x01U) == 0U) return true;
    } while ((uint32_t)(HAL_GetTick() - start) < timeout_ms);
    return false;
}

static bool spi_flash_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    if (buf == NULL) return false;
    s_spi_ok = true;
    cs_low();
    (void)spi_transfer(0x03U);
    (void)spi_transfer((uint8_t)(addr >> 16));
    (void)spi_transfer((uint8_t)(addr >> 8));
    (void)spi_transfer((uint8_t)addr);
    if (!s_spi_ok) {
        cs_high();
        return false;
    }
    for (uint32_t i = 0U; i < len; ++i) {
        if (!s_spi_ok) break;
        buf[i] = spi_transfer(0xFFU);
    }
    cs_high();
    return s_spi_ok;
}

static bool spi_flash_write_enable(void)
{
    s_spi_ok = true;
    cs_low();
    (void)spi_transfer(0x06U);
    cs_high();
    return s_spi_ok;
}

static bool spi_flash_erase_sector(uint32_t addr)
{
    if (!spi_flash_write_enable()) return false;
    s_spi_ok = true;
    cs_low();
    (void)spi_transfer(0x20U);
    (void)spi_transfer((uint8_t)(addr >> 16));
    (void)spi_transfer((uint8_t)(addr >> 8));
    (void)spi_transfer((uint8_t)addr);
    cs_high();
    return s_spi_ok && spi_flash_wait_busy(SPI_FLASH_BUSY_TIMEOUT_MS);
}

static bool spi_flash_write_page(uint32_t addr, const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0U || len > SPI_FLASH_PAGE_SIZE ||
        ((addr & 0xFFU) + len > SPI_FLASH_PAGE_SIZE) || !spi_flash_write_enable()) {
        return false;
    }
    s_spi_ok = true;
    cs_low();
    (void)spi_transfer(0x02U);
    (void)spi_transfer((uint8_t)(addr >> 16));
    (void)spi_transfer((uint8_t)(addr >> 8));
    (void)spi_transfer((uint8_t)addr);
    for (uint16_t i = 0U; i < len; ++i) (void)spi_transfer(data[i]);
    cs_high();
    return s_spi_ok && spi_flash_wait_busy(SPI_FLASH_BUSY_TIMEOUT_MS);
}

static bool spi_flash_write(uint32_t addr, const uint8_t *data, uint32_t len)
{
    while (len > 0U) {
        uint32_t page_remaining = SPI_FLASH_PAGE_SIZE - (addr & 0xFFU);
        uint16_t part = (uint16_t)((len < page_remaining) ? len : page_remaining);
        if (!spi_flash_write_page(addr, data, part)) return false;
        addr += part;
        data += part;
        len -= part;
    }
    return true;
}

static bool spi_flash_erase_region(uint32_t base, uint32_t size)
{
    for (uint32_t offset = 0U; offset < size; offset += SPI_FLASH_SECTOR_SIZE) {
        if (!spi_flash_erase_sector(base + offset)) return false;
    }
    return true;
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0U; i < len; ++i) {
        crc ^= data[i];
        for (uint32_t bit = 0U; bit < 8U; ++bit) {
            crc = (crc >> 1U) ^ (0xEDB88320U & (0U - (crc & 1U)));
        }
    }
    return crc;
}

static uint32_t crc32_external(uint32_t address, uint32_t len)
{
    uint8_t chunk[256];
    uint32_t crc = 0xFFFFFFFFU;
    while (len > 0U) {
        uint32_t part = (len > sizeof(chunk)) ? sizeof(chunk) : len;
        if (!spi_flash_read(address, chunk, part)) return 0U;
        crc = crc32_update(crc, chunk, part);
        address += part;
        len -= part;
    }
    return crc ^ 0xFFFFFFFFU;
}

static uint32_t crc32_internal(uint32_t address, uint32_t len)
{
    return crc32_update(0xFFFFFFFFU, (const uint8_t *)address, len) ^ 0xFFFFFFFFU;
}

static uint32_t descriptor_crc(const OtaDescriptor_t *desc)
{
    return crc32_update(0xFFFFFFFFU, (const uint8_t *)desc,
                        sizeof(*desc) - sizeof(desc->header_crc32)) ^ 0xFFFFFFFFU;
}

static bool valid_descriptor(const OtaDescriptor_t *desc)
{
    return desc->magic == OTA_MAGIC_HEADER &&
           desc->descriptor_version == OTA_DESCRIPTOR_VERSION &&
           desc->target_mcu == OTA_TARGET_STM32G0B1 &&
           desc->header_crc32 == descriptor_crc(desc);
}

static bool read_descriptor_at(uint32_t address, OtaDescriptor_t *desc)
{
    return spi_flash_read(address, (uint8_t *)desc, sizeof(*desc)) && valid_descriptor(desc);
}

static bool save_descriptor(OtaDescriptor_t *desc)
{
    OtaDescriptor_t primary;
    OtaDescriptor_t mirror;
    bool primary_ok = read_descriptor_at(SPI_FLASH_OTA_META_BASE, &primary);
    bool mirror_ok = read_descriptor_at(SPI_FLASH_OTA_META_MIRROR_BASE, &mirror);
    uint32_t target = SPI_FLASH_OTA_META_BASE;
    uint32_t latest = 0U;

    if (primary_ok && (!mirror_ok || primary.transaction_id >= mirror.transaction_id)) {
        target = SPI_FLASH_OTA_META_MIRROR_BASE;
    } else if (mirror_ok) {
        target = SPI_FLASH_OTA_META_BASE;
    }
    if (primary_ok && primary.transaction_id > latest) latest = primary.transaction_id;
    if (mirror_ok && mirror.transaction_id > latest) latest = mirror.transaction_id;
    desc->transaction_id = latest + 1U;
    desc->header_crc32 = descriptor_crc(desc);
    return spi_flash_erase_sector(target) &&
           spi_flash_write(target, (const uint8_t *)desc, sizeof(*desc));
}

static bool load_descriptor(OtaDescriptor_t *desc)
{
    OtaDescriptor_t primary;
    OtaDescriptor_t mirror;
    bool primary_ok = read_descriptor_at(SPI_FLASH_OTA_META_BASE, &primary);
    bool mirror_ok = read_descriptor_at(SPI_FLASH_OTA_META_MIRROR_BASE, &mirror);
    if (primary_ok && mirror_ok) *desc = (primary.transaction_id >= mirror.transaction_id) ? primary : mirror;
    else if (primary_ok) *desc = primary;
    else if (mirror_ok) *desc = mirror;
    else return false;
    return true;
}

static bool app_vector_valid(void)
{
    uint32_t sp = *(__IO uint32_t *)APP_START_ADDR;
    uint32_t entry = *(__IO uint32_t *)(APP_START_ADDR + 4U);
    return (sp >= 0x20000000U && sp <= 0x20024000U) &&
           ((entry & 1U) != 0U) &&
           (entry >= APP_START_ADDR && entry < (APP_START_ADDR + OTA_MAX_IMAGE_SIZE));
}

static bool backup_current_application(OtaDescriptor_t *desc)
{
    uint8_t chunk[256];
    uint32_t crc = 0xFFFFFFFFU;
    if (!app_vector_valid() || !spi_flash_erase_region(SPI_FLASH_OTA_BACKUP_BASE,
                                                       SPI_FLASH_OTA_BACKUP_SIZE)) {
        return false;
    }
    for (uint32_t offset = 0U; offset < OTA_MAX_IMAGE_SIZE; offset += sizeof(chunk)) {
        memcpy(chunk, (const void *)(APP_START_ADDR + offset), sizeof(chunk));
        if (!spi_flash_write(SPI_FLASH_OTA_BACKUP_BASE + offset, chunk, sizeof(chunk))) return false;
        crc = crc32_update(crc, chunk, sizeof(chunk));
        IWDG->KR = 0xAAAAU;
    }
    desc->backup_image_size = OTA_MAX_IMAGE_SIZE;
    desc->backup_crc32 = crc ^ 0xFFFFFFFFU;
    if (crc32_external(SPI_FLASH_OTA_BACKUP_BASE, OTA_MAX_IMAGE_SIZE) != desc->backup_crc32) return false;
    desc->backup_valid = OTA_BACKUP_VALID_MAGIC;
    return save_descriptor(desc);
}

static bool copy_external_to_application(const OtaDescriptor_t *desc, uint32_t source_base)
{
    uint8_t chunk[256];
    uint32_t copied = 0U;
    uint32_t pages = (desc->image_size + STM32G0_FLASH_PAGE_SIZE - 1U) /
                     STM32G0_FLASH_PAGE_SIZE;
    FLASH_EraseInitTypeDef erase = {0};
    uint32_t page_error = 0U;

    erase.TypeErase = FLASH_TYPEERASE_PAGES;
    erase.Banks = FLASH_BANK_1;
    erase.Page = APP_START_PAGE;
    erase.NbPages = pages;
    IWDG->KR = 0xAAAAU;
    if (HAL_FLASHEx_Erase(&erase, &page_error) != HAL_OK) return false;

    while (copied < desc->image_size) {
        uint32_t part = desc->image_size - copied;
        if (part > sizeof(chunk)) part = sizeof(chunk);
        memset(chunk, 0xFF, sizeof(chunk));
        if (!spi_flash_read(source_base + copied, chunk, part)) return false;
        for (uint32_t i = 0U; i < sizeof(chunk); i += 8U) {
            uint64_t dword;
            memcpy(&dword, &chunk[i], 8U);
            if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD,
                                  APP_START_ADDR + copied + i, dword) != HAL_OK) return false;
        }
        copied += part;
        IWDG->KR = 0xAAAAU;
    }
    return crc32_internal(APP_START_ADDR, desc->image_size) == desc->image_crc32 && app_vector_valid();
}

static bool restore_backup(OtaDescriptor_t *desc)
{
    OtaDescriptor_t backup_desc = *desc;
    backup_desc.image_size = OTA_MAX_IMAGE_SIZE;
    backup_desc.image_crc32 = desc->backup_crc32;
    if (!copy_external_to_application(&backup_desc, SPI_FLASH_OTA_BACKUP_BASE)) return false;
    desc->backup_valid = 0U;
    desc->boot_request = OTA_BOOT_FLAG_CLEARED;
    desc->status = OTA_STATUS_ERROR_ROLLBACK;
    desc->health_marker = OTA_HEALTH_MARKER_CONFIRMED;
    return save_descriptor(desc);
}

static void jump_to_application(void)
{
    uint32_t app_sp = *(__IO uint32_t *)APP_START_ADDR;
    uint32_t app_entry = *(__IO uint32_t *)(APP_START_ADDR + 4U);
    if (app_vector_valid()) {
        HAL_SPI_DeInit(&hspi2);
        __disable_irq();
        SysTick->CTRL = 0U;
        SysTick->LOAD = 0U;
        SysTick->VAL = 0U;
        NVIC->ICER[0] = 0xFFFFFFFFU;
        NVIC->ICPR[0] = 0xFFFFFFFFU;
        SCB->VTOR = APP_START_ADDR;
        IWDG->KR = 0xAAAAU;

        __asm__ volatile(
            "msr msp, %0\n"
            "msr control, %2\n"
            "isb\n"
            "dsb\n"
            "cpsie i\n"
            "bx %1\n"
            :
            : "r"(app_sp), "r"(app_entry), "r"(0)
            : "memory"
        );
    }
    while (1) {
        HAL_GPIO_TogglePin(LED_PORT, LED_PIN);
        for (volatile uint32_t i = 0U; i < 200000U; ++i) { }
    }
}

int main(void)
{
    OtaDescriptor_t desc;
    HAL_Init();
    SystemClock_Config();
    GPIO_Init();
    SPI2_Init();

    cs_low();
    (void)spi_transfer(0xABU);
    cs_high();

    if (load_descriptor(&desc) && desc.image_size > 0U &&
        desc.image_size <= OTA_MAX_IMAGE_SIZE) {
        if (desc.status == OTA_STATUS_VERIFIED &&
            desc.boot_request == OTA_BOOT_FLAG_REQUEST) {
            HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_SET);
            if (crc32_external(SPI_FLASH_OTA_STAGING_BASE, desc.image_size) != desc.image_crc32) {
                desc.status = OTA_STATUS_ERROR_CRC;
                desc.boot_request = OTA_BOOT_FLAG_CLEARED;
                (void)save_descriptor(&desc);
            } else if (desc.backup_valid != OTA_BACKUP_VALID_MAGIC &&
                       !backup_current_application(&desc)) {
                desc.status = OTA_STATUS_ERROR_FLASH;
                desc.boot_request = OTA_BOOT_FLAG_CLEARED;
                (void)save_descriptor(&desc);
            } else {
                HAL_FLASH_Unlock();
                if (copy_external_to_application(&desc, SPI_FLASH_OTA_STAGING_BASE)) {
                    desc.boot_request = OTA_BOOT_FLAG_CLEARED;
                    desc.status = OTA_STATUS_BOOT_TEST;
                    desc.boot_attempts = 0U;
                    desc.health_marker = OTA_HEALTH_MARKER_PENDING;
                    (void)save_descriptor(&desc);
                } else {
                    desc.status = OTA_STATUS_ERROR_FLASH;
                    desc.boot_request = OTA_BOOT_FLAG_CLEARED;
                    (void)save_descriptor(&desc);
                }
                HAL_FLASH_Lock();
            }
            HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_RESET);
        } else if (desc.status == OTA_STATUS_BOOT_TEST &&
                   desc.boot_request == OTA_BOOT_FLAG_CLEARED) {
            if (desc.health_marker == OTA_HEALTH_MARKER_CONFIRMED) {
                desc.status = OTA_STATUS_APPLIED;
                desc.boot_attempts = 0U;
                (void)save_descriptor(&desc);
            } else if (desc.boot_attempts >= OTA_BOOT_ATTEMPT_LIMIT) {
                HAL_FLASH_Unlock();
                if (!restore_backup(&desc)) {
                    desc.status = OTA_STATUS_ERROR_ROLLBACK;
                    (void)save_descriptor(&desc);
                }
                HAL_FLASH_Lock();
            } else {
                ++desc.boot_attempts;
                (void)save_descriptor(&desc);
            }
        }
    }

    jump_to_application();
    return 0;
}
