/**
 * @file    debug_log.h
 * @brief   Debug logging qua UART1 (PA9 TX, 115200 baud)
 * @note    Dùng USB-TTL cắm header J6 trên board để xem log.
 *          Nhẹ, không dùng malloc, buffer tĩnh 128 bytes.
 */

#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#include <stdint.h>

/**
 * @brief  In chuỗi format ra UART1 (giống printf)
 * @note   Max 128 ký tự mỗi lần gọi. Blocking (đợi gửi xong).
 */
void LOG(const char *fmt, ...);

/**
 * @brief  In banner khởi động (gọi 1 lần trong App_Init)
 */
void LOG_Banner(void);
void LOG_TxCpltCallback(void);

#endif /* DEBUG_LOG_H */

