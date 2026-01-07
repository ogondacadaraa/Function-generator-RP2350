# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico2 CACHE STRING "Board type")
set(PICO_PLATFORM rp2350 CACHE STRING "Platform")

cmake_minimum_required(VERSION 3.13)

# Initialize the SDK
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(FUNCTION_generator C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

# Define the project executable
add_executable(FUNCTION_generator
    FUNCTION_generator.cpp
)

# Process the PIO file
pico_generate_pio_header(FUNCTION_generator ${CMAKE_CURRENT_LIST_DIR}/dac.pio)

# Link the libraries
target_link_libraries(FUNCTION_generator 
    pico_stdlib 
    hardware_pio 
    hardware_dma 
    hardware_vreg
    hardware_clocks
)

# Enable USB, disable UART
pico_enable_stdio_usb(FUNCTION_generator 1)
pico_enable_stdio_uart(FUNCTION_generator 0)

pico_add_extra_outputs(FUNCTION_generator)