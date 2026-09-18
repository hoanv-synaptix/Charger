target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/alarm.c
    ${CMAKE_CURRENT_LIST_DIR}/alarm_storage.c
    ${CMAKE_CURRENT_LIST_DIR}/dwin_alarm_text.c
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)
