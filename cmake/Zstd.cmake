# zstd, shared by the desktop host and the Android guest.
#
# Both ends need it: the host compresses a batch of tiles, the phone
# decompresses it. Kept here rather than in either CMakeLists so the two builds
# cannot end up on different versions of the format -- they are separate CMake
# invocations with separate build directories, and the only thing tying them
# together is this file naming one tag.
#
# Fetched rather than vendored. Vendoring zstd means several hundred files in
# the tree for something neither end modifies, and the guest build already
# reaches outside its own directory for common/.

include_guard(GLOBAL)

include(FetchContent)

set(ZSTD_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_SHARED   OFF CACHE BOOL "" FORCE)
set(ZSTD_BUILD_STATIC   ON  CACHE BOOL "" FORCE)
set(ZSTD_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
# Nothing here reads archives written by a zstd older than this one.
set(ZSTD_LEGACY_SUPPORT OFF CACHE BOOL "" FORCE)

FetchContent_Declare(zstd
    GIT_REPOSITORY https://github.com/facebook/zstd.git
    GIT_TAG        v1.5.6
    GIT_SHALLOW    TRUE
    # The project's CMakeLists is not at the top of the repository.
    SOURCE_SUBDIR  build/cmake
)

FetchContent_MakeAvailable(zstd)

# Third-party code does not have to meet our warning bar, and zstd is built
# with settings we did not choose.
if(TARGET libzstd_static)
    if(MSVC)
        target_compile_options(libzstd_static PRIVATE /W0)
    else()
        target_compile_options(libzstd_static PRIVATE -w)
    endif()

    # The installed package exports zstd::libzstd_static; a FetchContent build
    # does not, so the alias is made here and both ends spell it the same way.
    if(NOT TARGET zstd::libzstd_static)
        add_library(zstd::libzstd_static ALIAS libzstd_static)
    endif()
endif()
