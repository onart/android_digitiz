# PointerPipeline is pure logic behind IInputInjector, so it is tested against
# a fake injector without dragging in ImGui, GLFW, or the Win32 backend.
#
# The key-name table is the exception: virtual-key numbering is the host OS's,
# so the table has to come from the platform file. It is self-contained enough
# that pulling it in costs only windows.h.

add_executable(digitiz_host_tests
    main.cpp
    test_absolute_coord.cpp
    test_etc2.cpp
    test_key_names.cpp
    test_latency_stats.cpp
    test_pointer_pipeline.cpp
    test_send_queue.cpp
    test_spline_smoother.cpp
    test_tile_scheduler.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/input/PointerPipeline.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/input/SplineSmoother.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/input/Win32KeyNames.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/screen/Etc2.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/transport/SendQueue.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/screen/TileScheduler.cpp
)

target_include_directories(digitiz_host_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(digitiz_host_tests PRIVATE digitiz::common doctest::doctest)

add_test(NAME host COMMAND digitiz_host_tests)
