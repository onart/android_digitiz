# PointerPipeline is pure logic behind IInputInjector, so it is tested against
# a fake injector without dragging in ImGui, GLFW, or the Win32 backend.

add_executable(digitiz_host_tests
    main.cpp
    test_absolute_coord.cpp
    test_pointer_pipeline.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../src/input/PointerPipeline.cpp
)

target_include_directories(digitiz_host_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(digitiz_host_tests PRIVATE digitiz::common doctest::doctest)

add_test(NAME host COMMAND digitiz_host_tests)
