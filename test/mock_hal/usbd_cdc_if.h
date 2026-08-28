#ifndef MOCK_USBD_CDC_IF_H
#define MOCK_USBD_CDC_IF_H
/* Minimal stand-in for USB_Device/App/usbd_cdc_if.h + the ST USB Device
 * Library types it pulls in transitively -- just enough for
 * App/Protocol/pc_protocol.c to compile and link on host. pc_protocol.c
 * only touches hUsbDeviceFS.pClassData(Cmsit)/hcdc->TxState/CDC_Transmit_FS();
 * it never needs the real USB stack's actual behavior, so this stub keeps
 * every USB transfer a no-op success (see test/mock_hal/mock_stubs.c). */
#include <stdint.h>

typedef enum {
    USBD_OK = 0U,
    USBD_BUSY,
    USBD_FAIL
} USBD_StatusTypeDef;

#define USBD_MAX_SUPPORTED_CLASS 1U

typedef struct {
    volatile uint32_t TxState;
} USBD_CDC_HandleTypeDef;

typedef struct {
    void *pClassData;
    void *pClassDataCmsit[USBD_MAX_SUPPORTED_CLASS];
} USBD_HandleTypeDef;

extern USBD_HandleTypeDef hUsbDeviceFS;

uint8_t CDC_Transmit_FS(uint8_t *Buf, uint16_t Len);

#endif /* MOCK_USBD_CDC_IF_H */
