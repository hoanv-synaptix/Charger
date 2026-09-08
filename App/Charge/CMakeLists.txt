target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/charge_controller.c
    ${CMAKE_CURRENT_LIST_DIR}/charge_cycle_config.c
    ${CMAKE_CURRENT_LIST_DIR}/charge_cycle_storage.c
    ${CMAKE_CURRENT_LIST_DIR}/charge_energy_storage.c
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)
