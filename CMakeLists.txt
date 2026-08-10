cmake_minimum_required(VERSION 3.24)
project(fidget VERSION 0.1.0 LANGUAGES C CXX)

# C++17: the oldest toolchains this tool must build on stop there.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# GCC 8 implements C++17 filesystem in a separate compatibility library.
# Keep this off fidget_core itself so the core architecture boundary remains
# explicit, and place it after the core archive at final link sites.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
   AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    set(FIDGET_CXX17_FILESYSTEM_LIBRARY stdc++fs)
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# So editors and clang tooling can find the vendored headers.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(vendor)
add_subdirectory(src)

enable_testing()
add_subdirectory(tests)
