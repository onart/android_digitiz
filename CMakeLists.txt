cmake_minimum_required(VERSION 3.22)

project(digitiz
    VERSION 0.1.0
    DESCRIPTION "Android phone as a PC digitizer"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(DIGITIZ_BUILD_HOST  "Build the PC host application" ON)
option(DIGITIZ_BUILD_TESTS "Build common/ unit tests"      ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

if(DIGITIZ_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(common)

# host/ is added in Phase 2; guard so the tree builds before it exists.
if(DIGITIZ_BUILD_HOST AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/host/CMakeLists.txt")
    add_subdirectory(host)
endif()
