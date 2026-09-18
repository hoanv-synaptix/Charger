/**
 * @file    bsp_quectel.h
 * @brief   Board Support Package - Quectel LTE / 4G Module Driver (USART5, PB5, PD3)
 * @note    Non-blocking hardware driver for Quectel modules (EC200U / EC25 / EG915...)
 */

#ifndef BSP_QUECTEL_H
#define BSP_QUECTEL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    QUECTEL_PWR_OFF = 0,
    QUECTEL_PWR_STABILIZING,    /* PB5 HIGH, waiting power rail stability */
    QUECTEL_PWR_PULSING_KEY,    /* PD3 HIGH, asserting PWRKEY */
    QUECTEL_PWR_WAITING_BOOT,   /* PD3 LOW, waiting for modem UART boot */
    QUECTEL_PWR_READY,          /* Modem is powered and ready for AT commands */
    QUECTEL_PWR_SHUTDOWN_PULSE, /* Asserting PWRKEY to shut down */
    QUECTEL_PWR_SHUTDOWN_WAIT   /* Waiting for power off */
} QuectelPowerState_t;

/**
 * @brief Initialize Quectel hardware pins and USART5 RX interrupt.
 */
void BSP_Quectel_Init(void);

/**
 * @brief Request the modem to power on (non-blocking).
 */
void BSP_Quectel_PowerOn(void);

/**
 * @brief Request the modem to power off (non-blocking).
 * @param graceful If true, sends shutdown pulse; if false, immediately cuts PB5.
 */
void BSP_Quectel_PowerOff(bool graceful);

/**
 * @brief Periodic process function for power state machine.
 *        Call in main loop with current tick.
 */
void BSP_Quectel_Process(uint32_t now_tick);

/**
 * @brief Check if modem is powered on and ready for AT communication.
 */
bool BSP_Quectel_IsReady(void);

/**
 * @brief Get current hardware power state.
 */
QuectelPowerState_t BSP_Quectel_GetPowerState(void);

/**
 * @brief Send raw binary/AT data via USART5.
 */
bool BSP_Quectel_Send(const uint8_t *data, uint16_t len);

/**
 * @brief Send null-terminated AT command string via USART5 (appends nothing).
 */
bool BSP_Quectel_SendString(const char *str);

/**
 * @brief Send AT command with \r\n appended.
 */
bool BSP_Quectel_SendCmd(const char *cmd);

/**
 * @brief Read available received bytes from RX ring buffer.
 * @return Number of bytes copied to dest.
 */
uint16_t BSP_Quectel_Read(uint8_t *dest, uint16_t max_len);

/**
 * @brief Read a single line (up to \n or max_len-1) from RX buffer.
 * @param line Output buffer (will be null-terminated).
 * @param max_len Maximum length of buffer including null terminator.
 * @return true if a complete line was extracted, false otherwise.
 */
bool BSP_Quectel_ReadLine(char *line, uint16_t max_len);

/**
 * @brief Clear RX ring buffer.
 */
void BSP_Quectel_ClearRx(void);

/**
 * @brief Check how many bytes are currently available in RX buffer.
 */
uint16_t BSP_Quectel_Available(void);

/**
 * @brief UART callbacks called from app_it_callbacks.c
 */
void BSP_Quectel_RxCpltCallback(void *huart);
void BSP_Quectel_ErrorCallback(void *huart);

#ifdef __cplusplus
}
#endif

#endif /* BSP_QUECTEL_H */
