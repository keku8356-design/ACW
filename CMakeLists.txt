# Aircover - ESP32-C3 HomeKit air-duct controller
# Targets ESP-IDF v5.5.4
cmake_minimum_required(VERSION 3.16)

# Pull in esp-homekit-sdk components.
# Expected layout (clone with submodules into the parent of this project):
#   <workspace>/esp-homekit-sdk/                <- the SDK
#   <workspace>/aircover/                       <- this project
# Override with:  idf.py -DHOMEKIT_PATH=/abs/path build
set(HOMEKIT_PATH "${CMAKE_SOURCE_DIR}/../esp-homekit-sdk" CACHE PATH "Path to esp-homekit-sdk")

if(NOT EXISTS "${HOMEKIT_PATH}/components/homekit")
    message(FATAL_ERROR
        "esp-homekit-sdk not found at ${HOMEKIT_PATH}.\n"
        "Run from this project's parent directory:\n"
        "  git clone --recursive https://github.com/espressif/esp-homekit-sdk.git\n"
        "Or pass -DHOMEKIT_PATH=/your/path to idf.py."
    )
endif()

list(APPEND EXTRA_COMPONENT_DIRS
    "${HOMEKIT_PATH}/components/homekit"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(aircover)
