target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    $<$<BOOL:${CHG_ENABLE_SD_CARD}>:${CMAKE_CURRENT_LIST_DIR}/sd_diskio.c>
    $<$<BOOL:${CHG_ENABLE_SD_CARD}>:${CMAKE_CURRENT_LIST_DIR}/sd_storage.c>
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)
