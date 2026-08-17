cmake_minimum_required(VERSION 3.22)

project(digitiz_guest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The shared core, compiled straight from the repo rather than vendored. Same
# sources the PC host builds; see common/CMakeLists.txt for the no-platform-
# headers rule that makes this possible.
set(DIGITIZ_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(
    ${CMAKE_CURRENT_SOURCE_DIR}/../../../../../common
    ${CMAKE_CURRENT_BINARY_DIR}/digitiz_common
)

find_package(game-activity REQUIRED CONFIG)

add_library(digitiz_guest SHARED
    App.cpp
    main.cpp
    input/TouchRouter.cpp
    net/TcpTransport.cpp
    render/GlContext.cpp
    render/GridRenderer.cpp
    render/Shader.cpp
    render/UiRenderer.cpp
    ui/SideMenu.cpp
)

target_include_directories(digitiz_guest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_options(digitiz_guest PRIVATE -Wall -Wextra)

# GameActivity's Java half reaches into native code through JNI
# (initializeNativeCode and friends). Nothing in our own code references those
# symbols, so a normal static link drops them and the app dies at startup with
# UnsatisfiedLinkError. --whole-archive forces every member of the archive into
# our .so, which is where JNI looks for them.
target_link_libraries(digitiz_guest
    PRIVATE
        -Wl,--whole-archive
        game-activity::game-activity_static
        -Wl,--no-whole-archive
        digitiz::common
        android
        log
        EGL
        GLESv3
)
