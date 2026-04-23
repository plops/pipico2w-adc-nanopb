cmake_minimum_required(VERSION 3.13)

# Include the Pico SDK import file
include(pico_sdk_import.cmake)

project(PicoDAQ C CXX)

# Initialize the Pico SDK
pico_sdk_init()

# --- Security: Import Wi-Fi credentials from CMake definitions ---
if(NOT DEFINED WIFI_SSID)
    set(WIFI_SSID "DEFAULT_SSID")
endif()
if(NOT DEFINED WIFI_PASS)
    set(WIFI_PASS "DEFAULT_PASS")
endif()

# --- Nanopb Setup ---
# nanopb_generate_cpp requires nanopb_BUILD_GENERATOR to be ON
set(nanopb_BUILD_GENERATOR ON)
add_subdirectory(nanopb)

# Generate C++ files from the proto definition
nanopb_generate_cpp(PROTO_SRCS PROTO_HDRS instrument.proto)

# --- Executable Setup ---
add_executable(daq_app 
    main.cpp 
    ${PROTO_SRCS}
)

# Require C++20 for std::span, std::array, structured bindings
target_compile_features(daq_app PRIVATE cxx_std_20)

# Pass the Wi-Fi secrets into the C++ code as macros
target_compile_definitions(daq_app PRIVATE
    WIFI_SSID="${WIFI_SSID}"
    WIFI_PASS="${WIFI_PASS}"
)

# Include directories
target_include_directories(daq_app PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}
)

# Link hardware libraries, multicore, LWIP networking, and Nanopb
target_link_libraries(daq_app PRIVATE
    pico_stdlib
    pico_multicore
    hardware_adc
    hardware_dma
    pico_cyw43_arch_lwip_threadsafe_background
    protobuf-nanopb
)

# Enable USB output, disable UART
pico_enable_stdio_usb(daq_app 1)
pico_enable_stdio_uart(daq_app 0)

# Add extra outputs (bin, uf2, etc.)
pico_add_extra_outputs(daq_app)
