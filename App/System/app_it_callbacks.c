#include "main.h"
#include "usart.h"
#include "debug_log.h"
#include "bsp_rs485.h"
#include "bsp_quectel.h"

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

void HAL_UART_RxCpltCallback(UART_HandleTypeDef *huart)
{
    if (huart == NULL) return;

    if (huart->Instance == USART3) {
        BSP_RS485_RxCpltCallback(huart);
    } else if (huart->Instance == USART5) {
        BSP_Quectel_RxCpltCallback(huart);
    }
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef *huart)
{
    if (huart == NULL) return;

    if (huart->Instance == USART3) {
        BSP_RS485_ErrorCallback(huart);
    } else if (huart->Instance == USART5) {
        BSP_Quectel_ErrorCallback(huart);
    }
}


