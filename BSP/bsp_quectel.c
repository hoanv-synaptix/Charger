/**
 * @file    bsp_quectel.c
 * @brief   Board Support Package - Quectel LTE / 4G Module Driver (USART5, PB5, PD3)
 * @note    Non-blocking hardware driver for Quectel modules (EC200U / EC25 / EG915...)
 */

#include "bsp_quectel.h"
#include "main.h"
#include "usart.h"
#include "debug_log.h"
#include <string.h>

/* Timing configurations (in milliseconds) */
#define TIMEOUT_POWER_STABILIZE_MS      200U    /* Wait after PB5 HIGH before pulsing PWRKEY */
#define TIMEOUT_PWRKEY_PULSE_MS         1200U   /* Duration of PWRKEY pulse to turn ON */
#define TIMEOUT_BOOT_SETTLE_MS          3000U   /* Wait after PWRKEY release for modem boot */
#define TIMEOUT_SHUTDOWN_PULSE_MS       1000U   /* Duration of PWRKEY pulse to turn OFF */
#define TIMEOUT_SHUTDOWN_SETTLE_MS      2000U   /* Wait before cutting PB5 power */

/* UART RX Ring Buffer Size */
#define QUECTEL_RX_BUF_SIZE             2048U

/* Private State */
static volatile QuectelPowerState_t s_pwr_state = QUECTEL_PWR_OFF;
static uint32_t s_state_start_tick = 0U;

static uint8_t s_rx_buf[QUECTEL_RX_BUF_SIZE];
static volatile uint16_t s_rx_tail = 0U;
static volatile uint32_t s_rx_overflow_count = 0U;
static volatile uint32_t s_rx_line_overflow_count = 0U;
static volatile bool s_discarding_line = false;
static bool s_rx_started = false;

static inline uint16_t get_rx_head(void)
{
    if (huart5.hdmarx == NULL) return 0U;
    uint16_t cndtr = (uint16_t)__HAL_DMA_GET_COUNTER(huart5.hdmarx);
    if (cndtr > QUECTEL_RX_BUF_SIZE) return 0U;
    return (uint16_t)((QUECTEL_RX_BUF_SIZE - cndtr) % QUECTEL_RX_BUF_SIZE);
}

/* Hardware control helpers */
static inline void set_pwr_en(bool enable)
{
    HAL_GPIO_WritePin(MCU_PB5_LTE_PWR_EN_GPIO_Port, MCU_PB5_LTE_PWR_EN_Pin,
                      enable ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static inline void set_pwrkey(bool active)
{
    HAL_GPIO_WritePin(MCU_PD3_LTE_PWRKEY_GPIO_Port, MCU_PD3_LTE_PWRKEY_Pin,
                      active ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void BSP_Quectel_Init(void)
{
    /* Ensure power is initially OFF and PWRKEY is inactive */
    set_pwrkey(false);
    set_pwr_en(false);
    s_pwr_state = QUECTEL_PWR_OFF;
    s_state_start_tick = 0U;

    s_rx_tail = 0U;
    s_rx_overflow_count = 0U;

    /* Start USART5 circular DMA RX */
    if (!s_rx_started) {
        if (HAL_UART_Receive_DMA(&huart5, s_rx_buf, QUECTEL_RX_BUF_SIZE) == HAL_OK) {
            s_rx_started = true;
            LOG("Quectel: Circular DMA RX started (%u bytes buffer)\r\n", QUECTEL_RX_BUF_SIZE);
        } else {
            LOG("Quectel: ERROR starting Circular DMA RX\r\n");
        }
    }
}

void BSP_Quectel_PowerOn(void)
{
    if (s_pwr_state == QUECTEL_PWR_READY || s_pwr_state == QUECTEL_PWR_WAITING_BOOT) {
        return; /* Already powered or powering up */
    }

    /* Start power up sequence */
    set_pwrkey(false);
    set_pwr_en(true);
    s_pwr_state = QUECTEL_PWR_STABILIZING;
    s_state_start_tick = HAL_GetTick();
    LOG("Quectel: Power rail enabled, stabilizing...\r\n");
}

void BSP_Quectel_PowerOff(bool graceful)
{
    if (s_pwr_state == QUECTEL_PWR_OFF) {
        return;
    }

    if (graceful && s_pwr_state == QUECTEL_PWR_READY) {
        set_pwrkey(true);
        s_pwr_state = QUECTEL_PWR_SHUTDOWN_PULSE;
        s_state_start_tick = HAL_GetTick();
        LOG("Quectel: Graceful shutdown initiated...\r\n");
    } else {
        /* Hard cut */
        set_pwrkey(false);
        set_pwr_en(false);
        s_pwr_state = QUECTEL_PWR_OFF;
        LOG("Quectel: Hard power cut\r\n");
    }
}

void BSP_Quectel_Process(uint32_t now_tick)
{
    uint32_t elapsed = now_tick - s_state_start_tick;

    switch (s_pwr_state) {
    case QUECTEL_PWR_OFF:
        /* Nothing to do */
        break;

    case QUECTEL_PWR_STABILIZING:
        if (elapsed >= TIMEOUT_POWER_STABILIZE_MS) {
            /* Assert PWRKEY */
            set_pwrkey(true);
            s_pwr_state = QUECTEL_PWR_PULSING_KEY;
            s_state_start_tick = now_tick;
            LOG("Quectel: Asserting PWRKEY...\r\n");
        }
        break;

    case QUECTEL_PWR_PULSING_KEY:
        if (elapsed >= TIMEOUT_PWRKEY_PULSE_MS) {
            /* Release PWRKEY */
            set_pwrkey(false);
            s_pwr_state = QUECTEL_PWR_WAITING_BOOT;
            s_state_start_tick = now_tick;
            LOG("Quectel: Released PWRKEY, waiting boot settle...\r\n");
        }
        break;

    case QUECTEL_PWR_WAITING_BOOT:
        if (elapsed >= TIMEOUT_BOOT_SETTLE_MS) {
            s_pwr_state = QUECTEL_PWR_READY;
            LOG("Quectel: Hardware READY for communication\r\n");
        }
        break;

    case QUECTEL_PWR_READY:
        /* Normal operation */
        break;

    case QUECTEL_PWR_SHUTDOWN_PULSE:
        if (elapsed >= TIMEOUT_SHUTDOWN_PULSE_MS) {
            set_pwrkey(false);
            s_pwr_state = QUECTEL_PWR_SHUTDOWN_WAIT;
            s_state_start_tick = now_tick;
        }
        break;

    case QUECTEL_PWR_SHUTDOWN_WAIT:
        if (elapsed >= TIMEOUT_SHUTDOWN_SETTLE_MS) {
            set_pwr_en(false);
            s_pwr_state = QUECTEL_PWR_OFF;
            LOG("Quectel: Graceful shutdown complete\r\n");
        }
        break;

    default:
        s_pwr_state = QUECTEL_PWR_OFF;
        break;
    }
}

bool BSP_Quectel_IsReady(void)
{
    return (s_pwr_state == QUECTEL_PWR_READY);
}

QuectelPowerState_t BSP_Quectel_GetPowerState(void)
{
    return s_pwr_state;
}

bool BSP_Quectel_Send(const uint8_t *data, uint16_t len)
{
    if (data == NULL || len == 0U) {
        return false;
    }
    return (HAL_UART_Transmit(&huart5, (uint8_t *)data, len, 500U) == HAL_OK);
}

bool BSP_Quectel_SendString(const char *str)
{
    if (str == NULL) {
        return false;
    }
    return BSP_Quectel_Send((const uint8_t *)str, (uint16_t)strlen(str));
}

bool BSP_Quectel_SendCmd(const char *cmd)
{
    if (cmd == NULL) {
        return false;
    }
    if (!BSP_Quectel_SendString(cmd)) {
        return false;
    }
    return BSP_Quectel_SendString("\r\n");
}

uint16_t BSP_Quectel_Read(uint8_t *dest, uint16_t max_len)
{
    if (dest == NULL || max_len == 0U) {
        return 0U;
    }

    uint16_t head = get_rx_head();
    uint16_t count = 0U;
    while (s_rx_tail != head && count < max_len) {
        dest[count++] = s_rx_buf[s_rx_tail];
        s_rx_tail = (uint16_t)((s_rx_tail + 1U) % QUECTEL_RX_BUF_SIZE);
    }
    return count;
}

bool BSP_Quectel_ReadLine(char *line, uint16_t max_len)
{
    if (line == NULL || max_len < 2U) {
        return false;
    }

    uint16_t head = get_rx_head();

    /* Phase 1: If currently in discard mode, swallow bytes up to '\n' */
    if (s_discarding_line) {
        while (s_rx_tail != head) {
            uint8_t b = s_rx_buf[s_rx_tail];
            s_rx_tail = (uint16_t)((s_rx_tail + 1U) % QUECTEL_RX_BUF_SIZE);
            if (b == '\n') {
                s_discarding_line = false;
                break;
            }
        }
        if (s_discarding_line) {
            return false; /* Still discarding unclosed long line */
        }
    }

    /* Phase 2: Scan buffer for newline '\n' without exceeding max_len-1 */
    uint16_t tail = s_rx_tail;
    bool found_newline = false;
    uint16_t line_len = 0U;

    while (tail != head) {
        uint8_t b = s_rx_buf[tail];
        tail = (uint16_t)((tail + 1U) % QUECTEL_RX_BUF_SIZE);
        line_len++;
        if (b == '\n') {
            found_newline = true;
            break;
        }
        if (line_len >= (max_len - 1U)) {
            /* Line is too long for destination buffer.
             * Discard all bytes scanned so far and enter discard mode until '\n' */
            s_discarding_line = true;
            ++s_rx_line_overflow_count;
            s_rx_tail = tail;

            /* Opportunistically scan the rest of already-buffered bytes for '\n' */
            while (s_rx_tail != head) {
                uint8_t db = s_rx_buf[s_rx_tail];
                s_rx_tail = (uint16_t)((s_rx_tail + 1U) % QUECTEL_RX_BUF_SIZE);
                if (db == '\n') {
                    s_discarding_line = false;
                    break;
                }
            }
            return false;
        }
    }

    if (!found_newline) {
        return false;
    }

    /* Extract line up to '\n' */
    uint16_t out_idx = 0U;
    while (s_rx_tail != head) {
        uint8_t b = s_rx_buf[s_rx_tail];
        s_rx_tail = (uint16_t)((s_rx_tail + 1U) % QUECTEL_RX_BUF_SIZE);

        if (b == '\r') {
            continue; /* Strip CR */
        }
        if (b == '\n') {
            break; /* End of line reached */
        }
        if (out_idx < (max_len - 1U)) {
            line[out_idx++] = (char)b;
        }
    }
    line[out_idx] = '\0';
    return true;
}

void BSP_Quectel_ClearRx(void)
{
    s_rx_tail = get_rx_head();
    s_discarding_line = false;
}

uint32_t BSP_Quectel_GetLineOverflowCount(void)
{
    return s_rx_line_overflow_count;
}

uint16_t BSP_Quectel_Available(void)
{
    uint16_t head = get_rx_head();
    if (head >= s_rx_tail) {
        return (uint16_t)(head - s_rx_tail);
    }
    return (uint16_t)(QUECTEL_RX_BUF_SIZE - (s_rx_tail - head));
}

uint32_t BSP_Quectel_GetRxOverflowCount(void)
{
    return s_rx_overflow_count;
}

void BSP_Quectel_RxCpltCallback(void *huart)
{
    /* In circular DMA mode, hardware wraps automatically. No action needed. */
    (void)huart;
}

void BSP_Quectel_ErrorCallback(void *huart)
{
    UART_HandleTypeDef *uart = (UART_HandleTypeDef *)huart;
    if (uart != NULL && uart->Instance == USART5) {
        __HAL_UART_CLEAR_FLAG(uart, UART_CLEAR_OREF | UART_CLEAR_FEF |
                                    UART_CLEAR_NEF | UART_CLEAR_PEF);
        /* If circular DMA stopped due to error, restart it */
        if (uart->hdmarx != NULL && uart->hdmarx->State != HAL_DMA_STATE_BUSY) {
            HAL_UART_Receive_DMA(&huart5, s_rx_buf, QUECTEL_RX_BUF_SIZE);
        }
    }
}
