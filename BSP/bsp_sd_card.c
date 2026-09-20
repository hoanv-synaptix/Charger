/**
 * @file    bsp_sd_card.c
 * @brief   SPI-mode SD card block driver for SPI1.
 *
 * The driver deliberately exposes sectors, not files. FatFs owns filesystem
 * semantics; this module only owns SD protocol, card addressing and bounded
 * SPI transactions.
 */

#include "bsp_sd_card.h"

#include "main.h"
#include "spi.h"

#include <limits.h>
#include <stddef.h>

#define SD_COMMAND_TIMEOUT_MS       1000U
#define SD_BLOCK_TIMEOUT_MS         200U
#define SD_SPI_TIMEOUT_MS           20U
#define SD_INIT_PRESCALER           SPI_BAUDRATEPRESCALER_256
#define SD_DATA_PRESCALER           SPI_BAUDRATEPRESCALER_4

#define SD_CMD0                     0U
#define SD_CMD8                     8U
#define SD_CMD9                     9U
#define SD_CMD16                    16U
#define SD_CMD17                    17U
#define SD_CMD24                    24U
#define SD_CMD41                    41U
#define SD_CMD55                    55U
#define SD_CMD58                    58U
#define SD_CMD59                    59U

#define SD_R1_IDLE                  0x01U
#define SD_R1_ILLEGAL_COMMAND      0x04U
#define SD_DATA_TOKEN               0xFEU
#define SD_DATA_ACCEPTED_MASK      0x1FU
#define SD_DATA_ACCEPTED            0x05U

static bool s_ready;
static SD_CardType_t s_type;
static uint32_t s_block_count;
static SD_IoStage_t s_last_read_stage;
static uint8_t s_last_read_response;
static bool s_initializing;
static uint32_t s_init_start_tick;

static void sd_cs_low(void)
{
    HAL_GPIO_WritePin(MCU_PB0_SPI1_CS_GPIO_Port, MCU_PB0_SPI1_CS_Pin,
                      GPIO_PIN_RESET);
}

static void sd_cs_high(void)
{
    HAL_GPIO_WritePin(MCU_PB0_SPI1_CS_GPIO_Port, MCU_PB0_SPI1_CS_Pin,
                      GPIO_PIN_SET);
}

static bool sd_spi_byte(uint8_t tx, uint8_t *rx)
{
    uint8_t received = 0xFFU;

    if (HAL_SPI_TransmitReceive(&hspi1, &tx, &received, 1U,
                                SD_SPI_TIMEOUT_MS) != HAL_OK) {
        return false;
    }

    if (rx != NULL) {
        *rx = received;
    }
    return true;
}

static bool sd_spi_receive(uint8_t *data, uint16_t length)
{
    if (data == NULL || length == 0U) {
        return false;
    }
    for (uint16_t i = 0U; i < length; ++i) {
        if (!sd_spi_byte(0xFFU, &data[i])) {
            return false;
        }
    }
    return true;
}

static bool sd_spi_transmit(const uint8_t *data, uint16_t length)
{
    if (data == NULL || length == 0U) {
        return false;
    }
    return HAL_SPI_Transmit(&hspi1, (uint8_t *)data, length,
                            SD_SPI_TIMEOUT_MS) == HAL_OK;
}

static uint8_t sd_crc7(const uint8_t *data, uint8_t length)
{
    uint8_t crc = 0U;

    for (uint8_t i = 0U; i < length; ++i) {
        uint8_t value = data[i];
        for (uint8_t bit = 0U; bit < 8U; ++bit) {
            crc <<= 1U;
            if (((crc ^ value) & 0x80U) != 0U) {
                crc ^= 0x09U;
            }
            value <<= 1U;
        }
    }
    return (uint8_t)(crc & 0x7FU);
}

static bool sd_change_speed(uint32_t prescaler)
{
    sd_cs_high();
    hspi1.Init.BaudRatePrescaler = prescaler;
    return HAL_SPI_Init(&hspi1) == HAL_OK;
}

static bool sd_wait_r1(uint8_t *response)
{
    const uint32_t start = HAL_GetTick();
    uint32_t timeout = SD_COMMAND_TIMEOUT_MS;
    uint8_t value = 0xFFU;

    if (s_initializing) {
        const uint32_t elapsed = (uint32_t)(start - s_init_start_tick);
        if (elapsed >= SD_COMMAND_TIMEOUT_MS) {
            return false;
        }
        timeout = SD_COMMAND_TIMEOUT_MS - elapsed;
    }

    do {
        if (!sd_spi_byte(0xFFU, &value)) {
            return false;
        }
        if ((value & 0x80U) == 0U) {
            if (response != NULL) {
                *response = value;
            }
            return true;
        }
    } while ((uint32_t)(HAL_GetTick() - start) < timeout);

    return false;
}

/* CS remains asserted after this function so callers can consume R2/R3/R7 or
 * a data token without creating an extra command transaction. */
static bool sd_send_command_selected(uint8_t command, uint32_t argument,
                                     uint8_t *response)
{
    uint8_t packet[5];

    packet[0] = (uint8_t)(0x40U | command);
    packet[1] = (uint8_t)(argument >> 24U);
    packet[2] = (uint8_t)(argument >> 16U);
    packet[3] = (uint8_t)(argument >> 8U);
    packet[4] = (uint8_t)argument;

    if (!sd_spi_transmit(packet, sizeof(packet))) {
        return false;
    }

    {
        uint8_t crc_packet[1];
        crc_packet[0] = (uint8_t)((sd_crc7(packet, sizeof(packet)) << 1U) | 1U);
        if (!sd_spi_transmit(crc_packet, sizeof(crc_packet))) {
            return false;
        }
    }

    return sd_wait_r1(response);
}

static bool sd_send_command(uint8_t command, uint32_t argument,
                            uint8_t *response)
{
    bool ok;

    sd_cs_low();
    ok = sd_send_command_selected(command, argument, response);
    sd_cs_high();
    (void)sd_spi_byte(0xFFU, NULL);
    return ok;
}

static bool sd_wait_data_token(uint8_t expected)
{
    const uint32_t start = HAL_GetTick();
    uint32_t timeout = SD_BLOCK_TIMEOUT_MS;
    uint8_t token = 0xFFU;

    if (s_initializing) {
        const uint32_t elapsed = (uint32_t)(start - s_init_start_tick);
        if (elapsed >= SD_COMMAND_TIMEOUT_MS) {
            return false;
        }
        timeout = SD_COMMAND_TIMEOUT_MS - elapsed;
        if (timeout > SD_BLOCK_TIMEOUT_MS) {
            timeout = SD_BLOCK_TIMEOUT_MS;
        }
    }

    do {
        if (!sd_spi_byte(0xFFU, &token)) {
            return false;
        }
        if (token == expected) {
            return true;
        }
        if (token != 0xFFU) {
            return false;
        }
    } while ((uint32_t)(HAL_GetTick() - start) < timeout);

    return false;
}

static bool sd_wait_not_busy(uint32_t timeout_ms)
{
    const uint32_t start = HAL_GetTick();
    uint8_t value = 0x00U;

    do {
        if (!sd_spi_byte(0xFFU, &value)) {
            return false;
        }
        if (value == 0xFFU) {
            return true;
        }
    } while ((uint32_t)(HAL_GetTick() - start) < timeout_ms);

    return false;
}

static bool sd_card_is_v2(void)
{
    uint8_t response = 0xFFU;
    uint8_t r7[4];

    sd_cs_low();
    if (!sd_send_command_selected(SD_CMD8, 0x000001AAUL, &response)) {
        sd_cs_high();
        (void)sd_spi_byte(0xFFU, NULL);
        return false;
    }

    if (response & SD_R1_ILLEGAL_COMMAND) {
        sd_cs_high();
        (void)sd_spi_byte(0xFFU, NULL);
        return false;
    }

    if (!sd_spi_receive(r7, sizeof(r7))) {
        sd_cs_high();
        (void)sd_spi_byte(0xFFU, NULL);
        return false;
    }
    sd_cs_high();
    (void)sd_spi_byte(0xFFU, NULL);

    return response == SD_R1_IDLE && r7[2] == 0x01U && r7[3] == 0xAAU;
}

static bool sd_send_acmd41(bool high_capacity)
{
    uint8_t response = 0xFFU;

    do {
        if (!sd_send_command(SD_CMD55, 0U, &response)) {
            return false;
        }
        if ((response & (uint8_t)~SD_R1_IDLE) != 0U &&
            response != 0x00U) {
            return false;
        }

        if (!sd_send_command(SD_CMD41, high_capacity ? 0x40000000UL : 0U,
                             &response)) {
            return false;
        }
        if (response == 0x00U) {
            return true;
        }
        if (response != SD_R1_IDLE) {
            return false;
        }
    } while ((uint32_t)(HAL_GetTick() - s_init_start_tick) <
             SD_COMMAND_TIMEOUT_MS);

    return false;
}

static bool sd_read_csd(uint8_t csd[16])
{
    uint8_t response = 0xFFU;
    uint8_t crc[2];
    bool ok = false;

    sd_cs_low();
    if (sd_send_command_selected(SD_CMD9, 0U, &response) &&
        response == 0x00U && sd_wait_data_token(SD_DATA_TOKEN) &&
        sd_spi_receive(csd, 16U) && sd_spi_receive(crc, sizeof(crc))) {
        ok = true;
    }
    sd_cs_high();
    (void)sd_spi_byte(0xFFU, NULL);
    return ok;
}

static uint32_t sd_get_bits(const uint8_t *data, uint8_t msb, uint8_t lsb)
{
    uint32_t value = 0U;

    for (uint8_t bit = msb; bit >= lsb; --bit) {
        const uint8_t byte_index = (uint8_t)(15U - (bit / 8U));
        const uint8_t bit_index = (uint8_t)(bit % 8U);
        value = (value << 1U) | ((data[byte_index] >> bit_index) & 1U);
        if (bit == 0U) {
            break;
        }
    }
    return value;
}

static uint32_t sd_decode_block_count(const uint8_t csd[16])
{
    const uint32_t structure = sd_get_bits(csd, 127U, 126U);
    uint64_t blocks;

    if (structure == 1U) {
        const uint32_t c_size = sd_get_bits(csd, 69U, 48U);
        blocks = ((uint64_t)c_size + 1ULL) * 1024ULL;
    } else if (structure == 0U) {
        const uint32_t read_bl_len = sd_get_bits(csd, 83U, 80U);
        const uint32_t c_size = sd_get_bits(csd, 73U, 62U);
        const uint32_t c_size_mult = sd_get_bits(csd, 49U, 47U);
        const uint32_t shift = c_size_mult + 2U +
                               ((read_bl_len >= 9U) ? (read_bl_len - 9U) : 0U);

        blocks = ((uint64_t)c_size + 1ULL) << shift;
        if (read_bl_len < 9U) {
            blocks >>= (9U - read_bl_len);
        }
    } else {
        return 0U;
    }

    return blocks > UINT32_MAX ? UINT32_MAX : (uint32_t)blocks;
}

static bool sd_valid_range(uint32_t block, uint32_t count)
{
    return s_ready && s_block_count != 0U && count != 0U &&
           block < s_block_count && count <= (s_block_count - block);
}

static uint32_t sd_argument_for_block(uint32_t block)
{
    if (s_type == SD_TYPE_SDHC) {
        return block;
    }
    return block * SD_BLOCK_SIZE;
}

bool BSP_SDCard_IsPresent(void)
{
    /* Q600 inverter circuit: CD closed to GND when card is inserted ->
     * Q600 turns OFF -> R600 pulls PB2 HIGH (GPIO_PIN_SET). */
    return HAL_GPIO_ReadPin(MCU_PB2_MicroSD_DT_GPIO_Port,
                            MCU_PB2_MicroSD_DT_Pin) == GPIO_PIN_SET;
}

bool BSP_SDCard_Init(void)
{
    uint8_t response = 0xFFU;
    uint8_t ocr[4];
    uint8_t csd[16];
    bool v2;

    s_ready = false;
    s_type = SD_TYPE_NONE;
    s_block_count = 0U;
    s_last_read_stage = SD_IO_STAGE_NONE;
    s_last_read_response = 0xFFU;
    s_initializing = false;

    if (!BSP_SDCard_IsPresent() || !sd_change_speed(SD_INIT_PRESCALER)) {
        return false;
    }

    s_init_start_tick = HAL_GetTick();
    s_initializing = true;

    sd_cs_high();
    for (uint8_t i = 0U; i < 10U; ++i) {
        if (!sd_spi_byte(0xFFU, NULL)) {
            s_initializing = false;
            return false;
        }
    }

    {
        const uint32_t start = HAL_GetTick();
        do {
            if (sd_send_command(SD_CMD0, 0U, &response) &&
                response == SD_R1_IDLE) {
                break;
            }
        } while ((uint32_t)(HAL_GetTick() - start) < SD_COMMAND_TIMEOUT_MS);
        if (response != SD_R1_IDLE) {
            s_initializing = false;
            return false;
        }
    }

    v2 = sd_card_is_v2();
    if (!sd_send_acmd41(v2)) {
        s_initializing = false;
        return false;
    }

    sd_cs_low();
    if (!sd_send_command_selected(SD_CMD58, 0U, &response) ||
        response != 0x00U || !sd_spi_receive(ocr, sizeof(ocr))) {
        sd_cs_high();
        (void)sd_spi_byte(0xFFU, NULL);
        s_initializing = false;
        return false;
    }
    sd_cs_high();
    (void)sd_spi_byte(0xFFU, NULL);

    s_type = (v2 && (ocr[0] & 0x40U)) ? SD_TYPE_SDHC : SD_TYPE_SDSC;

    /* SD cards power up with CRC disabled. CMD59 is best-effort: valid
     * command CRC is still sent for every command, while data CRC is not
     * required in this lean driver. */
    (void)sd_send_command(SD_CMD59, 0U, &response);

    if (s_type == SD_TYPE_SDSC &&
        (!sd_send_command(SD_CMD16, SD_BLOCK_SIZE, &response) ||
         response != 0x00U)) {
        s_type = SD_TYPE_NONE;
        s_initializing = false;
        return false;
    }

    if (!sd_read_csd(csd)) {
        s_type = SD_TYPE_NONE;
        s_initializing = false;
        return false;
    }
    s_block_count = sd_decode_block_count(csd);
    if (s_block_count == 0U || !sd_change_speed(SD_DATA_PRESCALER)) {
        s_type = SD_TYPE_NONE;
        s_block_count = 0U;
        s_initializing = false;
        return false;
    }

    s_ready = true;
    s_initializing = false;
    return true;
}

bool BSP_SDCard_IsReady(void)
{
    return s_ready && BSP_SDCard_IsPresent();
}

SD_CardType_t BSP_SDCard_GetType(void)
{
    return s_type;
}

uint32_t BSP_SDCard_GetBlockCount(void)
{
    return s_ready ? s_block_count : 0U;
}

SD_IoStage_t BSP_SDCard_GetLastReadStage(void)
{
    return s_last_read_stage;
}

uint8_t BSP_SDCard_GetLastReadResponse(void)
{
    return s_last_read_response;
}

bool BSP_SDCard_ReadBlock(uint32_t block, uint8_t *buf)
{
    uint8_t response = 0xFFU;
    uint8_t crc[2];
    bool ok = false;

    if (!sd_valid_range(block, 1U) || buf == NULL ||
        !BSP_SDCard_IsPresent()) {
        s_last_read_stage = SD_IO_STAGE_NOT_READY;
        s_last_read_response = 0xFFU;
        return false;
    }

    sd_cs_low();
    {
        const bool command_ok = sd_send_command_selected(
            SD_CMD17, sd_argument_for_block(block), &response);
        s_last_read_response = response;
        if (!command_ok || response != 0x00U) {
        s_last_read_stage = SD_IO_STAGE_COMMAND;
        } else if (!sd_wait_data_token(SD_DATA_TOKEN)) {
        s_last_read_stage = SD_IO_STAGE_TOKEN;
        } else if (!sd_spi_receive(buf, SD_BLOCK_SIZE)) {
        s_last_read_stage = SD_IO_STAGE_DATA;
        } else if (!sd_spi_receive(crc, 2U)) {
        s_last_read_stage = SD_IO_STAGE_CRC;
        } else {
        ok = true;
        s_last_read_stage = SD_IO_STAGE_NONE;
        }
    }
    sd_cs_high();
    (void)sd_spi_byte(0xFFU, NULL);
    return ok;
}

static uint8_t s_last_write_cmd_resp = 0xFFU;
static uint8_t s_last_write_data_resp = 0xFFU;
static bool s_last_write_not_busy_ok = false;
static bool s_last_write_ok = false;

bool BSP_SDCard_WriteBlock(uint32_t block, const uint8_t *buf)
{
    uint8_t response = 0xFFU;
    uint8_t data_response = 0xFFU;
    const uint8_t token = SD_DATA_TOKEN;
    const uint8_t crc[2] = {0xFFU, 0xFFU};
    bool ok = false;

    s_last_write_ok = false;
    s_last_write_cmd_resp = 0xFFU;
    s_last_write_data_resp = 0xFFU;
    s_last_write_not_busy_ok = false;

    if (!sd_valid_range(block, 1U) || buf == NULL ||
        !BSP_SDCard_IsPresent()) {
        return false;
    }

    sd_cs_low();
    if (sd_send_command_selected(SD_CMD24, sd_argument_for_block(block),
                                  &response) && response == 0x00U) {
        s_last_write_cmd_resp = response;
        /* Send at least one dummy byte before start data token */
        (void)sd_spi_byte(0xFFU, NULL);

        if (sd_spi_transmit(&token, 1U) &&
            sd_spi_transmit(buf, SD_BLOCK_SIZE) &&
            sd_spi_transmit(crc, sizeof(crc))) {

            /* Poll for data response token (within 64 bytes) */
            for (uint8_t i = 0U; i < 64U; ++i) {
                if (sd_spi_byte(0xFFU, &data_response)) {
                    if ((data_response & 0x11U) == 0x01U) {
                        break;
                    }
                }
            }
            s_last_write_data_resp = data_response;

            if ((data_response & SD_DATA_ACCEPTED_MASK) == SD_DATA_ACCEPTED) {
                /* Wait at least a couple clocks for card to assert busy */
                for (uint8_t b = 0U; b < 8U; ++b) {
                    uint8_t dummy = 0xFFU;
                    (void)sd_spi_byte(0xFFU, &dummy);
                    if (dummy == 0x00U) break;
                }
                if (sd_wait_not_busy(1000U)) {
                    s_last_write_not_busy_ok = true;
                    ok = true;
                }
            }
        }
    } else {
        s_last_write_cmd_resp = response;
    }
    sd_cs_high();
    (void)sd_spi_byte(0xFFU, NULL);
    s_last_write_ok = ok;
    return ok;
}

bool BSP_SDCard_ReadBlocks(uint32_t block, uint8_t *buf, uint32_t count)
{
    if (!sd_valid_range(block, count) || buf == NULL) {
        return false;
    }

    for (uint32_t i = 0U; i < count; ++i) {
        if (!BSP_SDCard_ReadBlock(block + i, buf + (i * SD_BLOCK_SIZE))) {
            return false;
        }
    }
    return true;
}

bool BSP_SDCard_WriteBlocks(uint32_t block, const uint8_t *buf,
                            uint32_t count)
{
    if (!sd_valid_range(block, count) || buf == NULL) {
        return false;
    }

    for (uint32_t i = 0U; i < count; ++i) {
        if (!BSP_SDCard_WriteBlock(block + i,
                                   buf + (i * SD_BLOCK_SIZE))) {
            return false;
        }
    }
    return true;
}

bool BSP_SDCard_SelfTest(void)
{
    uint8_t block[SD_BLOCK_SIZE];

    return BSP_SDCard_IsReady() && BSP_SDCard_ReadBlock(0U, block) &&
           block[510] == 0x55U && block[511] == 0xAAU;
}
