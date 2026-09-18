cmake_minimum_required(VERSION 3.22)

#
# This file is generated only once,
# and is not re-generated if converter is called multiple times.
#
# User is free to modify the file as much as necessary
#

# Setup compiler settings
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)


# Define the build type
option(CHG_DEBUG_DWIN "Enable the USB->RS485 DWIN relay debug command (bench only)" OFF)
option(CHG_DEBUG_CAN_TX_TRACE "Trace MCU CAN1 control frames to UART1 (bench only)" OFF)
option(CHG_ENABLE_LOG "Enable UART1 debug logging (bench only)" OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Release")
endif()

# Set the project name
set(CMAKE_PROJECT_NAME Charger)

# Enable compile command to ease indexing with e.g. clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS TRUE)

# Core project settings
project(${CMAKE_PROJECT_NAME})
message("Build type: " ${CMAKE_BUILD_TYPE})

# Enable CMake support for ASM and C languages
enable_language(C ASM)

# Create an executable object type
add_executable(${CMAKE_PROJECT_NAME})

# Add STM32CubeMX generated sources
add_subdirectory(cmake/stm32cubemx)
add_subdirectory(BSP)
add_subdirectory(Modules)
add_subdirectory(App)
add_subdirectory(Utils)

# Link directories setup
target_link_directories(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined library search paths
)

# Add sources to executable
target_sources(${CMAKE_PROJECT_NAME} PRIVATE)

# Add include paths
target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE)

# Add project symbols (macros)
target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE
    # Add user defined symbols
    # CHG_DEBUG_RAW_CAN: enables DEBUG_CMD_SEND_RAW_CAN (App/Protocol/pc_debug_protocol.c),
    # which lets the PC debug app inject an arbitrary frame on either CAN bus,
    # bypassing BMS/chg_lib entirely. Bench-debug only -- deliberately left
    # out of Release so a production unit's USB debug port cannot be used to
    # send arbitrary commands onto the charger/BMS bus.
    $<$<CONFIG:Debug>:CHG_DEBUG_RAW_CAN>
    # CHG_DEBUG_DWIN: enables DEBUG_CMD_DWIN_XFER (App/Protocol/pc_debug_protocol.c
    # + App/Protocol/app_dwin_debug.c) -- the PC pushes a raw frame to the DWIN
    # panel over RS485 and reads the reply, via the MCU's own transceiver
    # (test/dwin_cli.py). OFF by default and NOT tied to CONFIG:Debug, because
    # the -O0 -g3 Debug build already overflows the 128 KB flash; turn it on for
    # a Release build:  cmake --preset Release -DCHG_DEBUG_DWIN=ON
    $<$<BOOL:${CHG_DEBUG_DWIN}>:CHG_DEBUG_DWIN>
    # CHG_DEBUG_CAN_TX_TRACE: captures successful MCU->charger-module control
    # frames (C_M_2/C_M_24) into a bounded queue and prints them from the main
    # loop through UART1. It also suppresses unrelated LOG() lines in this
    # diagnostic build. Never enable it for production: UART output is slow.
    $<$<BOOL:${CHG_DEBUG_CAN_TX_TRACE}>:CHG_DEBUG_CAN_TX_TRACE>
    # CHG_ENABLE_LOG: enables UART1 LOG() output for bench debugging.
    # OFF by default in production/Release to save ~8-9 KB Flash and eliminate UART blocking.
    $<$<BOOL:${CHG_ENABLE_LOG}>:CHG_ENABLE_LOG>
)

# Remove wrong libob.a library dependency when using cpp files
list(REMOVE_ITEM CMAKE_C_IMPLICIT_LINK_LIBRARIES ob)

# Add linked libraries
target_link_libraries(${CMAKE_PROJECT_NAME}
    stm32cubemx

    # Add user defined libraries
)

# Generate .hex and .bin files after build
add_custom_command(TARGET ${CMAKE_PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.hex
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${CMAKE_PROJECT_NAME}> ${CMAKE_PROJECT_NAME}.bin
    COMMENT "Generating .hex and .bin files"
)
