# This is a copy of <PICO_SDK_PATH>/external/pico_sdk_import.cmake

if (NOT DEFINED PICO_SDK_PATH)
    if (NOT DEFINED ENV{PICO_SDK_PATH})
        message(FATAL_ERROR "SDK location was not specified. Please set PICO_SDK_PATH or set PICO_SDK_PATH in the environment.")
    endif ()
    set(PICO_SDK_PATH $ENV{PICO_SDK_PATH})
endif ()

set(PICO_SDK_PATH "${PICO_SDK_PATH}" CACHE PATH "Path to the Raspberry Pi Pico SDK")

if (NOT EXISTS "${PICO_SDK_PATH}/pico_sdk_init.cmake")
    message(FATAL_ERROR "Directory specified by PICO_SDK_PATH does not contain pico_sdk_init.cmake")
endif ()

include("${PICO_SDK_PATH}/pico_sdk_init.cmake")
