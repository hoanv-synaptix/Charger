/**
 * @file    boot_main.c
 * @brief   STM32G0B1 OTA Bootloader
 * @note    Runs from 0x08000000 (8KB), checks External SPI Flash for OTA
 *          updates, flashes application at 0x08002000, and jumps to app.
 */

#include "stm32g0xx_hal.h"
#include "../App/OTA/ota_types.h"
#include <string.h>

#define APP_START_ADDR                  0x08002000U
#define APP_START_PAGE                  4U      /* Page 4 = 4 * 2048 = 0x08002000 */
#define STM32G0_FLASH_PAGE_SIZE         2048U   /* 2 KB per page */

/* SPI Flash memory map */
#define SPI_FLASH_OTA_META_BASE         0x00010000U /* Sector 16 (4 KB) */
#define SPI_FLASH_OTA_STAGING_BASE      0x00020000U /* Sectors 32..63 (128 KB) */

/* Hardware Pins */
#define SPI2_CS_PORT                    GPIOB
#define SPI2_CS_PIN                     GPIO_PIN_9
#define LED_PORT                        GPIOC
#define LED_PIN                         GPIO_PIN_13

static SPI_HandleTypeDef hspi2;

/* ================= Hardware Initializations ================= */

static void SystemClock_Config(void)
{
    RCC_OscInitTypeDef RCC_OscInitStruct = {0};
    RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

    /* Configure 16MHz HSI as sysclk (simple, fast boot, no PLL wait) */
    RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI;
    RCC_OscInitStruct.HSIState = RCC_HSI_ON;
    RCC_OscInitStruct.HSIDiv = RCC_HSI_DIV1;
    RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    HAL_RCC_OscConfig(&RCC_OscInitStruct);

    RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1;
    RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_HSI;
    RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
    RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
    HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_0);
}

static void GPIO_Init(void)
{
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    GPIO_InitTypeDef GPIO_InitStruct = {0};

    /* CS Pin (PB9) - Output High */
    HAL_GPIO_WritePin(SPI2_CS_PORT, SPI2_CS_PIN, GPIO_PIN_SET);
    GPIO_InitStruct.Pin = SPI2_CS_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    HAL_GPIO_Init(SPI2_CS_PORT, &GPIO_InitStruct);

    /* LED Status (PC13) */
    HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_RESET);
    GPIO_InitStruct.Pin = LED_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(LED_PORT, &GPIO_InitStruct);

    /* SPI2 Pins: PB8 SCK (AF1), PB6 MISO (AF4), PB7 MOSI (AF1) */
    GPIO_InitStruct.Pin = GPIO_PIN_8 | GPIO_PIN_7;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF1_SPI2;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);

    GPIO_InitStruct.Pin = GPIO_PIN_6;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF4_SPI2;
    HAL_GPIO_Init(GPIOB, &GPIO_InitStruct);
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
    hspi2.Init.BaudRatePrescaler = SPI_BAUDRATEPRESCALER_2; /* 8 MHz @ 16MHz clock */
    hspi2.Init.FirstBit = SPI_FIRSTBIT_MSB;
    hspi2.Init.TIMode = SPI_TIMODE_DISABLE;
    hspi2.Init.CRCCalculation = SPI_CRCCALCULATION_DISABLE;
    hspi2.Init.NSSPMode = SPI_NSS_PULSE_DISABLE;
    HAL_SPI_Init(&hspi2);
}

/* ================= SPI Flash Polling Driver ================= */

static inline void cs_low(void)  { HAL_GPIO_WritePin(SPI2_CS_PORT, SPI2_CS_PIN, GPIO_PIN_RESET); }
static inline void cs_high(void) { HAL_GPIO_WritePin(SPI2_CS_PORT, SPI2_CS_PIN, GPIO_PIN_SET); }

static uint8_t spi_transfer(uint8_t byte)
{
    uint8_t rx = 0xFFU;
    HAL_SPI_TransmitReceive(&hspi2, &byte, &rx, 1U, 100U);
    return rx;
}

static void spi_flash_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    cs_low();
    spi_transfer(0x03U); /* Read command */
    spi_transfer((uint8_t)(addr >> 16));
    spi_transfer((uint8_t)(addr >> 8));
    spi_transfer((uint8_t)addr);
    for (uint32_t i = 0; i < len; i++) {
        buf[i] = spi_transfer(0xFFU);
    }
    cs_high();
}

static void spi_flash_wait_busy(void)
{
    uint8_t status = 0;
    do {
        cs_low();
        spi_transfer(0x05U); /* Read Status Register 1 */
        status = spi_transfer(0xFFU);
        cs_high();
    } while ((status & 0x01U) != 0U);
}

static void spi_flash_write_enable(void)
{
    cs_low();
    spi_transfer(0x06U); /* Write Enable */
    cs_high();
}

static void spi_flash_erase_sector(uint32_t addr)
{
    spi_flash_write_enable();
    cs_low();
    spi_transfer(0x20U); /* Sector Erase 4KB */
    spi_transfer((uint8_t)(addr >> 16));
    spi_transfer((uint8_t)(addr >> 8));
    spi_transfer((uint8_t)addr);
    cs_high();
    spi_flash_wait_busy();
}

static void spi_flash_write_page(uint32_t addr, const uint8_t *data, uint16_t len)
{
    spi_flash_write_enable();
    cs_low();
    spi_transfer(0x02U); /* Page Program */
    spi_transfer((uint8_t)(addr >> 16));
    spi_transfer((uint8_t)(addr >> 8));
    spi_transfer((uint8_t)addr);
    for (uint16_t i = 0; i < len; i++) {
        spi_transfer(data[i]);
    }
    cs_high();
    spi_flash_wait_busy();
}

/* ================= Jump to Application ================= */

static void jump_to_application(void)
{
    uint32_t app_sp = *(__IO uint32_t *)APP_START_ADDR;

    /* Verify stack pointer resides within SRAM range (0x20000000 - 0x20024000) */
    if ((app_sp & 0x2FFE0000U) == 0x20000000U) {
        uint32_t app_entry = *(__IO uint32_t *)(APP_START_ADDR + 4U);
        void (*app_reset_handler)(void) = (void (*)(void))app_entry;

        /* Turn off peripherals */
        HAL_SPI_DeInit(&hspi2);
        HAL_RCC_DeInit();

        /* Disable all interrupts & SysTick */
        __disable_irq();
        SysTick->CTRL = 0;
        SysTick->LOAD = 0;
        SysTick->VAL = 0;

        /* Relocate vector table to Application offset */
        SCB->VTOR = APP_START_ADDR;

        /* Set MSP and jump */
        __set_MSP(app_sp);
        __enable_irq();
        app_reset_handler();
    }

    /* Fallback if application vector table is invalid: fast blink LED */
    while (1) {
        HAL_GPIO_TogglePin(LED_PORT, LED_PIN);
        for (volatile int i = 0; i < 200000; i++);
    }
}

/* ================= Main Bootloader Routine ================= */

int main(void)
{
    HAL_Init();
    SystemClock_Config();
    GPIO_Init();
    SPI2_Init();

    /* Release SPI Flash from Deep Power Down */
    cs_low();
    spi_transfer(0xABU);
    cs_high();
    for (volatile int i = 0; i < 5000; i++);

    /* Read OTA Descriptor from External SPI Flash */
    OtaDescriptor_t desc;
    spi_flash_read(SPI_FLASH_OTA_META_BASE, (uint8_t *)&desc, sizeof(desc));

    /* Check if an OTA update is requested */
    if (desc.magic == OTA_MAGIC_HEADER &&
        desc.boot_request == OTA_BOOT_FLAG_REQUEST &&
        desc.status == OTA_STATUS_VERIFIED &&
        desc.image_size > 0U && desc.image_size <= (120U * 1024U)) {

        /* Turn on LED to indicate flashing */
        HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_SET);

        /* Unlock STM32 Internal Flash */
        HAL_FLASH_Unlock();

        /* Calculate internal flash pages to erase */
        uint32_t pages_to_erase = (desc.image_size + STM32G0_FLASH_PAGE_SIZE - 1U) / STM32G0_FLASH_PAGE_SIZE;
        FLASH_EraseInitTypeDef erase_init;
        erase_init.TypeErase = FLASH_TYPEERASE_PAGES;
        erase_init.Banks = FLASH_BANK_1;
        erase_init.Page = APP_START_PAGE;
        erase_init.NbPages = pages_to_erase;

        uint32_t page_error = 0;
        if (HAL_FLASHEx_Erase(&erase_init, &page_error) == HAL_OK) {
            /* Copy firmware from SPI Flash staging area to Internal Flash */
            uint8_t chunk[256];
            uint32_t bytes_copied = 0U;
            bool copy_ok = true;

            while (bytes_copied < desc.image_size) {
                uint32_t chunk_len = desc.image_size - bytes_copied;
                if (chunk_len > sizeof(chunk)) {
                    chunk_len = sizeof(chunk);
                }

                /* Read from external flash */
                spi_flash_read(SPI_FLASH_OTA_STAGING_BASE + bytes_copied, chunk, chunk_len);

                /* Program into internal flash in 64-bit double words */
                for (uint32_t i = 0; i < chunk_len; i += 8U) {
                    uint64_t dword = 0xFFFFFFFFFFFFFFFFULL;
                    memcpy(&dword, &chunk[i], ((chunk_len - i) >= 8U) ? 8U : (chunk_len - i));

                    uint32_t target_addr = APP_START_ADDR + bytes_copied + i;
                    if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, target_addr, dword) != HAL_OK) {
                        copy_ok = false;
                        break;
                    }
                }

                if (!copy_ok) break;
                bytes_copied += chunk_len;
            }

            if (copy_ok) {
                /* Clear boot request and set applied status */
                desc.boot_request = OTA_BOOT_FLAG_CLEARED;
                desc.status = OTA_STATUS_APPLIED;

                /* Update descriptor in external flash */
                spi_flash_erase_sector(SPI_FLASH_OTA_META_BASE);
                spi_flash_write_page(SPI_FLASH_OTA_META_BASE, (const uint8_t *)&desc, sizeof(desc));
            }
        }

        HAL_FLASH_Lock();
        HAL_GPIO_WritePin(LED_PORT, LED_PIN, GPIO_PIN_RESET);
    }

    /* Jump to main Application */
    jump_to_application();

    return 0;
}
