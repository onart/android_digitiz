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

# One place for the version. The Android side carries its own in build.gradle.kts
# and the two have to be moved together, which is what the release checklist in
# README is for.
add_compile_definitions(DIGITIZ_VERSION="${PROJECT_VERSION}")

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus)
    # Static CRT, so the released executable runs on a machine that has never
    # had Visual Studio on it. Everything it links is built from source here,
    # so there is nothing to disagree with about the choice, and "install the
    # VC++ redistributable first" is a poor first impression.
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
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

# The guest is built by Gradle, not from here. This picks up only the parts of
# it that are plain C++, so they can be tested without a phone attached.
if(DIGITIZ_BUILD_TESTS)
    add_subdirectory(guest/tests)
endif()
