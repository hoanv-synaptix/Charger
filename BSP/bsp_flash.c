#include "bsp_flash.h"
#include "main.h"
#include <string.h>

bool BSP_Flash_ErasePage(uint32_t address) {
    uint32_t PageError = 0;
    FLASH_EraseInitTypeDef EraseInitStruct;
    
    if ((address % 8U) != 0U) return false;
    if (address < FLASH_BASE || address >= 0x08020000U) return false;
    
    // In G0, page is (addr - 0x08000000) / 2048
    uint32_t page = (address - FLASH_BASE) / FLASH_PAGE_SIZE;
    
    EraseInitStruct.TypeErase = FLASH_TYPEERASE_PAGES;
    EraseInitStruct.Page = page;
    EraseInitStruct.NbPages = 1;

    __disable_irq();
    HAL_FLASH_Unlock();
    HAL_StatusTypeDef st = HAL_FLASHEx_Erase(&EraseInitStruct, &PageError);
    HAL_FLASH_Lock();
    __enable_irq();
    if (st != HAL_OK) {
        return false;
    }
    return true;
}

bool BSP_Flash_WriteDoubleWord(uint32_t address, uint64_t data) {
    if ((address % 8U) != 0U) return false;
    if (address < FLASH_BASE || (address + 8U) > 0x08020000U) return false;
    __disable_irq();
    HAL_FLASH_Unlock();
    HAL_StatusTypeDef st = HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, address, data);
    HAL_FLASH_Lock();
    __enable_irq();
    if (st != HAL_OK) {
        return false;
    }
    return true;
}

bool BSP_Flash_WriteBlock(uint32_t address, const uint8_t *data, uint32_t len) {
    if ((address % 8U) != 0U) return false;
    if (address < FLASH_BASE || (address + len) > 0x08020000U) return false;
    if (data == NULL && len != 0) return false;
    __disable_irq();
    HAL_FLASH_Unlock();
    uint32_t end_addr = address + len;
    uint32_t current_addr = address;
    const uint8_t *ptr = data;
    
    while (current_addr < end_addr) {
        uint64_t double_word = 0xFFFFFFFFFFFFFFFFULL;
        uint32_t bytes_to_copy = (end_addr - current_addr) >= 8 ? 8 : (end_addr - current_addr);
        memcpy(&double_word, ptr, bytes_to_copy);
        
        if (HAL_FLASH_Program(FLASH_TYPEPROGRAM_DOUBLEWORD, current_addr, double_word) != HAL_OK) {
            HAL_FLASH_Lock();
            __enable_irq();
            return false;
        }
        current_addr += 8;
        ptr += 8;
    }
    HAL_FLASH_Lock();
    __enable_irq();
    return true;
}
