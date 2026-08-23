# The guest is an Android app, but not all of it is Android. ButtonStore is
# plain C++ over a text file, and it holds something the user typed and expects
# to still be there tomorrow, so it is worth testing where tests can run: the
# desktop toolchain compiles it straight out of the app tree.
#
# Anything that touches GLES or JNI stays out. That is why the strip's geometry
# is not here — it lives in a header that pulls in the renderers.

add_executable(digitiz_guest_tests
    main.cpp
    test_button_store.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/cpp/buttons/ButtonStore.cpp
)

target_include_directories(digitiz_guest_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../app/src/main/cpp)

target_link_libraries(digitiz_guest_tests PRIVATE digitiz::common doctest::doctest)

add_test(NAME guest COMMAND digitiz_guest_tests)
