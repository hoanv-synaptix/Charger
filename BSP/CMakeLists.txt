target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/bsp_rs485.c
    ${CMAKE_CURRENT_LIST_DIR}/bsp_can.c
    ${CMAKE_CURRENT_LIST_DIR}/bsp_flash.c
    ${CMAKE_CURRENT_LIST_DIR}/bsp_gpio.c
    ${CMAKE_CURRENT_LIST_DIR}/bsp_sys.c
    ${CMAKE_CURRENT_LIST_DIR}/bsp_failsafe.c
    ${CMAKE_CURRENT_LIST_DIR}/bsp_adc.c
    ${CMAKE_CURRENT_LIST_DIR}/bsp_rtc.c
    ${CMAKE_CURRENT_LIST_DIR}/bsp_spi_flash.c
    ${CMAKE_CURRENT_LIST_DIR}/bsp_sd_card.c
    ${CMAKE_CURRENT_LIST_DIR}/bsp_quectel.c
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)
