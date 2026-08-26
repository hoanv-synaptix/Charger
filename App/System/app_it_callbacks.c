#include "main.h"
#include "usart.h"
#include "debug_log.h"
#include "bsp_rs485.h"

void HAL_UART_TxCpltCallback(UART_HandleTypeDef *huart)
{
    /* 
    if (huart->Instance == USART1) {
        // LOG_TxCpltCallback();
    }
    else if (huart->Instance == USART3) {
        // BSP_RS485_TxCpltCallback();
    }
    */
}

