target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/chg_lib_core.c
    ${CMAKE_CURRENT_LIST_DIR}/chg_lib_can_backend.c
    ${CMAKE_CURRENT_LIST_DIR}/chg_lib_fsm.c
    ${CMAKE_CURRENT_LIST_DIR}/chg_lib_lianming.c
    ${CMAKE_CURRENT_LIST_DIR}/chg_lib_maxwell.c
    ${CMAKE_CURRENT_LIST_DIR}/chg_lib_tonhe.c
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}
)

