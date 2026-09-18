set(BOOT_TARGET Charger_Bootloader)

add_executable(${BOOT_TARGET}
    ${CMAKE_CURRENT_SOURCE_DIR}/boot_main.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Core/Src/system_stm32g0xx.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../startup_stm32g0b1xx.s
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_rcc.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_rcc_ex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_gpio.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_spi.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_flash.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_flash_ex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_cortex.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_dma.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_pwr.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32G0xx_HAL_Driver/Src/stm32g0xx_hal_pwr_ex.c
)

target_include_directories(${BOOT_TARGET} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../Core/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32G0xx_HAL_Driver/Inc
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/STM32G0xx_HAL_Driver/Inc/Legacy
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/CMSIS/Device/ST/STM32G0xx/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/../Drivers/CMSIS/Include
    ${CMAKE_CURRENT_SOURCE_DIR}/../App/OTA
)

target_compile_definitions(${BOOT_TARGET} PRIVATE
    USE_HAL_DRIVER
    STM32G0B1xx
)

target_compile_options(${BOOT_TARGET} PRIVATE
    -mcpu=cortex-m0plus
    -mthumb
    -Os
    -Wall
    -ffunction-sections
    -fdata-sections
)

target_link_options(${BOOT_TARGET} PRIVATE
    -T "${CMAKE_CURRENT_SOURCE_DIR}/STM32G0B1xx_BOOT.ld"
    -Wl,-Map=${BOOT_TARGET}.map
)

add_custom_command(TARGET ${BOOT_TARGET} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${BOOT_TARGET}> ${BOOT_TARGET}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${BOOT_TARGET}> ${BOOT_TARGET}.bin
    COMMENT "Generating Bootloader .hex and .bin files"
)
