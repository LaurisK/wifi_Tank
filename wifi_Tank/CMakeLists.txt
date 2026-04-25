cmake_minimum_required(VERSION 3.16)

# Add Trice component directory
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/Submodules/trice")

# Per-build-dir sdkconfig so esp32 and esp32s3 builds don't share one file.
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")

# This IDF version does not auto-load sdkconfig.defaults.<target>.
# IDF_TARGET is passed as -DIDF_TARGET=... by idf.py set-target, so it is
# available here before project.cmake includes it from the cache.
if(IDF_TARGET STREQUAL "esp32s3")
    set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.esp32s3")
else()
    set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.esp32")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wifi_Tank)