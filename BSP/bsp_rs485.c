#include "bsp_rs485.h"
#include "usart.h"
#include "main.h"

/* RS485 half-duplex: PB1 (DE) dieu huong phat/nhan.
 * Phan cung da dao 2 chan TX/RX USART3 -> da bat SWAP trong usart.c.
 * RX dung ring buffer: ISR chi push byte, main loop drain (BSP_RS485_Read). */

/* ===== DE polarity: PB1 HIGH = truyen (noi truc tiep chan DE) ===== */
#define DE_TX_ON    GPIO_PIN_SET
#define DE_TX_OFF   GPIO_PIN_RESET

static uint8_t rx_byte;  /* buffer 1 byte cho HAL_UART_Receive_IT */

#define RS485_RX_BUF_SIZE 128U
static uint8_t  rx_buf[RS485_RX_BUF_SIZE];
static volatile uint8_t rx_head = 0;
static volatile uint8_t rx_tail = 0;

void BSP_RS485_Init(void)
{
    /* DE muc LOW = che do nhan */
    HAL_GPIO_WritePin(GPIOB, MCU_PB1_UART_RTS_Pin, DE_TX_OFF);
    HAL_UART_Receive_IT(&huart3, &rx_byte, 1);
}

void UART_Transmit_To_DWIN(uint8_t* data, uint16_t len)
{
    if (len == 0 || data == 0) return;

    /* DE muc HIGH = che do truyen */
    HAL_GPIO_WritePin(GPIOB, MCU_PB1_UART_RTS_Pin, DE_TX_ON);
    
    /* Guard time (turn around time) truoc khi truyen */
    HAL_Delay(2);
    
    HAL_UART_Transmit(&huart3, data, len, 100);

    /* Tro ve muc LOW = che do nhan */
    HAL_GPIO_WritePin(GPIOB, MCU_PB1_UART_RTS_Pin, DE_TX_OFF);
}

void BSP_RS485_RxCpltCallback(void *huart)
{
    UART_HandleTypeDef *uart = (UART_HandleTypeDef *)huart;
    if (uart != NULL && uart->Instance == USART3) {
        uint8_t next = (uint8_t)((rx_head + 1U) % RS485_RX_BUF_SIZE);
        if (next != rx_tail) {
            rx_buf[rx_head] = rx_byte;
            rx_head = next;
        }
        /* Buffer day -> bo byte moi */
        HAL_UART_Receive_IT(&huart3, &rx_byte, 1);
    }
}

/* Re-arm RX khi co loi (ORE/FE/NE) - tranh chet kenh nhan sau nhieu */
void BSP_RS485_ErrorCallback(void *huart)
{
    UART_HandleTypeDef *uart = (UART_HandleTypeDef *)huart;
    if (uart != NULL && uart->Instance == USART3) {
        __HAL_UART_CLEAR_FLAG(uart, UART_CLEAR_OREF | UART_CLEAR_FEF |
                                     UART_CLEAR_NEF | UART_CLEAR_PEF);
        HAL_UART_Receive_IT(&huart3, &rx_byte, 1);
    }
}

uint16_t BSP_RS485_Read(uint8_t *out, uint16_t max_len)
{
    uint16_t n = 0;
    while (rx_tail != rx_head && n < max_len) {
        out[n++] = rx_buf[rx_tail];
        rx_tail = (uint8_t)((rx_tail + 1U) % RS485_RX_BUF_SIZE);
    }
    return n;
}
