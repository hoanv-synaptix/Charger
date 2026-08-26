#ifndef BSP_FLASH_H
#define BSP_FLASH_H

#include <stdint.h>
#include <stdbool.h>

#define BSP_CONFIG_FLASH_PAGE_ADDR 0x0801F800U // Page 63
#define BSP_FLASH_PAGE_SIZE 2048

bool BSP_Flash_ErasePage(uint32_t address);
bool BSP_Flash_WriteDoubleWord(uint32_t address, uint64_t data);
bool BSP_Flash_WriteBlock(uint32_t address, const uint8_t *data, uint32_t len);

#endif
