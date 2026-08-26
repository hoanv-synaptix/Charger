target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/bms_can.c
    ${CMAKE_CURRENT_LIST_DIR}/bms_core.c
    ${CMAKE_CURRENT_LIST_DIR}/bms_protocol.c
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

