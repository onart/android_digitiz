include(FetchContent)

FetchContent_Declare(doctest
    GIT_REPOSITORY https://github.com/doctest/doctest.git
    GIT_TAG        v2.4.11
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(doctest)

add_executable(digitiz_common_tests
    main.cpp
    test_messages.cpp
    test_framer.cpp
    test_geometry.cpp
)

target_link_libraries(digitiz_common_tests PRIVATE digitiz::common doctest::doctest)

add_test(NAME common COMMAND digitiz_common_tests)
