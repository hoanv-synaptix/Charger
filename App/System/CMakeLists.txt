target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/app_main.c
    ${CMAKE_CURRENT_LIST_DIR}/app_it_callbacks.c
    ${CMAKE_CURRENT_LIST_DIR}/app_rtc_sync.c
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

