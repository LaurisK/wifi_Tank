cmake_minimum_required(VERSION 3.16)

# Add Trice component directory
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/Submodules/trice")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wifi_Tank)