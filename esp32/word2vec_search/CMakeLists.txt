cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED IDF_TARGET)
    set(IDF_TARGET "esp32s3")
endif()

set(SDKCONFIG_DEFAULTS
    "sdkconfig.defaults"
    "sdkconfig.defaults.${IDF_TARGET}"
)

if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults.local")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.defaults.local")
endif()

get_filename_component(REPO_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(NN20DB_COMPONENT_DIR "${REPO_ROOT}/sdk/esp32/current/esp32/component/nn20db")

if(NOT EXISTS "${NN20DB_COMPONENT_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "ESP32 SDK not installed.\n"
        "Run from the nn20db-sdk repo root:\n"
        "  ./scripts/install-sdk.sh esp32 <tarball-url>\n"
        "Expected: ${NN20DB_COMPONENT_DIR}")
endif()

list(APPEND EXTRA_COMPONENT_DIRS "${NN20DB_COMPONENT_DIR}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nn20db_word2vec_search)
