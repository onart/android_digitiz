# digitiz_common — shared by the PC host and the Android guest.
#
# HARD RULE: no platform headers here. No <windows.h>, no <android/*.h>,
# no socket API. Standard library only. Both toolchains compile this verbatim.

add_library(digitiz_common STATIC
    src/log.cpp
    src/messages.cpp
)

target_include_directories(digitiz_common PUBLIC include)

target_compile_features(digitiz_common PUBLIC cxx_std_20)

add_library(digitiz::common ALIAS digitiz_common)

if(DIGITIZ_BUILD_TESTS)
    add_subdirectory(tests)
endif()
