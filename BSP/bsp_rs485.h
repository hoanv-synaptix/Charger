#ifndef BSP_RS485_H
#define BSP_RS485_H

#include <stdint.h>
#include <stdbool.h>

void BSP_RS485_Init(void);

/* Truyen data qua RS485 (tu dong dieu khien chan DE) */
void UART_Transmit_To_DWIN(uint8_t* data, uint16_t len);

/* Doc (va xoa khoi buffer) cac byte da nhan tu RS485.
 * Tra ve so byte da lay. Goi dinh ky trong main loop (cho DWIN). */
uint16_t BSP_RS485_Read(uint8_t *out, uint16_t max_len);
void BSP_RS485_TxCpltCallback(void);

#endif
