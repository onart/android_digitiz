include(FetchContent)

find_package(OpenGL REQUIRED)

set(GLFW_BUILD_DOCS     OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL        OFF CACHE BOOL "" FORCE)

FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG        3.4
    GIT_SHALLOW    TRUE
)

# imgui ships no CMakeLists, so MakeAvailable only downloads it and we build
# the translation units we actually want below.
FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5
    GIT_SHALLOW    TRUE
)

FetchContent_MakeAvailable(glfw imgui)

add_library(digitiz_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

target_include_directories(digitiz_imgui SYSTEM PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(digitiz_imgui PUBLIC glfw OpenGL::GL)

# Third-party code does not have to meet our warning bar.
if(MSVC)
    target_compile_options(digitiz_imgui PRIVATE /W0)
else()
    target_compile_options(digitiz_imgui PRIVATE -w)
endif()

# ---------------------------------------------------------------------------

add_library(digitiz_host_lib STATIC
    src/app/HostApp.cpp
    src/display/Win32DisplayInfo.cpp
    src/input/PointerPipeline.cpp
    src/input/SplineSmoother.cpp
    src/input/Win32KeyNames.cpp
    src/input/Win32InputInjector.cpp
    src/platform/Win32ForegroundWatcher.cpp
    src/platform/Win32Process.cpp
    src/screen/CaptureProbe.cpp
    src/screen/TileScheduler.cpp
    src/screen/Win32DxgiFrameSource.cpp
    src/transport/AdbClient.cpp
    src/transport/AdbTransport.cpp
    src/ui/ImGuiShell.cpp
)

target_include_directories(digitiz_host_lib PUBLIC src)
target_link_libraries(digitiz_host_lib PUBLIC digitiz::common digitiz_imgui)

if(WIN32)
    # Shcore: GetDpiForMonitor.  ws2_32: sockets.  Dwmapi: the visible window
    # frame, which is not what GetWindowRect reports.
    # d3d11/dxgi: Desktop Duplication.
    target_link_libraries(digitiz_host_lib PUBLIC Shcore ws2_32 Dwmapi d3d11 dxgi)
endif()

add_executable(digitiz_host src/main.cpp)
target_link_libraries(digitiz_host PRIVATE digitiz_host_lib)

if(DIGITIZ_BUILD_TESTS)
    add_subdirectory(tests)
endif()
