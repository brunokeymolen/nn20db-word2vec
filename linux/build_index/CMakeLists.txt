cmake_minimum_required(VERSION 3.16)
project(build_word2vec_index C)

set(CMAKE_C_STANDARD 11)

# ── SDK paths ─────────────────────────────────────────────────────────────────
set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
set(nn20db_DIR "${REPO_ROOT}/sdk/linux/current/cmake"
    CACHE PATH "Path to nn20dbConfig.cmake")

find_package(nn20db REQUIRED)
find_package(ZLIB REQUIRED)

add_executable(build_word2vec_index main.c)

target_compile_options(build_word2vec_index PRIVATE -O3 -Wall -Wextra)

target_link_libraries(build_word2vec_index PRIVATE
    nn20db::nn20db
    ZLIB::ZLIB
    m
)
