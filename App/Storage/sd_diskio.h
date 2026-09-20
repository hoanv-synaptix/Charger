/**
 * @file    sd_diskio.h
 * @brief   FatFs disk-I/O adapter for the SPI SD card BSP.
 */

#ifndef SD_DISKIO_H
#define SD_DISKIO_H

#include "ff.h"
#include "diskio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* FatFs calls the standard disk_* functions declared by diskio.h. This
 * header exists to make the adapter an explicit project-owned integration
 * point without adding a second storage abstraction. */

#ifdef __cplusplus
}
#endif

#endif /* SD_DISKIO_H */
