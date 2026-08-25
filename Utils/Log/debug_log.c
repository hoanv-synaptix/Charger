/**
 * @file    debug_log.c
 * @brief   Debug logging implementation qua USART1
 */

#include "debug_log.h"
#include "usart.h"
#include "main.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define LOG_BUF_SIZE  128

void LOG(const char *fmt, ...)
{
    char buf[LOG_BUF_SIZE];
    va_list args;
    va_start(args, fmt);
    int len = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    if (len > 0) {
        if (len > (int)(sizeof(buf) - 1)) len = sizeof(buf) - 1;
        HAL_UART_Transmit(&huart1, (uint8_t *)buf, (uint16_t)len, 50);
    }
}

void LOG_Banner(void)
{
    LOG("\r\n\r\n");
    LOG("========================================\r\n");
    LOG("  Charger Controller v2.0\r\n");
    LOG("  STM32G0B1CBT6 + multi-driver charger\r\n");
    LOG("  FDCAN1 @125K / FDCAN2 @250K\r\n");
    LOG("  USB CDC: PA11/PA12\r\n");
    LOG("  UART1 Debug: PA9 TX @ 115200\r\n");
    LOG("========================================\r\n");
}




/* Retarget printf() to UART1 */
int __io_putchar(int ch)
{
    HAL_UART_Transmit(&huart1, (uint8_t *)&ch, 1, 10);
    return ch;
}

int _write(int file, char *ptr, int len)
{
    int DataIdx;
    for (DataIdx = 0; DataIdx < len; DataIdx++)
    {
        __io_putchar(*ptr++);
    }
    return len;
}
