add_library(FatFS OBJECT
    ${CMAKE_CURRENT_LIST_DIR}/ff.c
)

target_include_directories(FatFS PUBLIC
    ${CMAKE_CURRENT_LIST_DIR}
)

target_compile_features(FatFS PUBLIC c_std_11)
