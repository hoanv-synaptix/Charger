/* Host-test stubs: LOG() passthrough to stdout, BSP_CAN_Transmit no-op. */
#include <stdio.h>
#include <stdarg.h>
#include "bsp_can.h"

void LOG(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

void LOG_Banner(void) {}
void LOG_TxCpltCallback(void) {}

bool BSP_CAN_Transmit(uint8_t bus, const BSP_CAN_Frame_t *frame)
{
    (void)bus;
    (void)frame;
    return true;
}
