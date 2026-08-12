cmake_minimum_required(VERSION 3.24)
project(fidget VERSION 0.1.0 LANGUAGES C CXX)

# C++17: the oldest toolchains this tool must build on stop there.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# GCC 8 implements C++17 filesystem in a separate compatibility library.
# src/core attaches it to fidget_core as a PUBLIC usage requirement so the
# linker always sees it after the core archive, for every consumer.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
   AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9)
    set(FIDGET_CXX17_FILESYSTEM_LIBRARY stdc++fs)
endif()

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# So editors and clang tooling can find the vendored headers.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# OFF builds only the core, hardware, CLI, and test targets. This lets the
# DAQ hosts build fidget_cli without X11 or OpenGL development packages.
option(FIDGET_BUILD_GUI "Build the desktop application" ON)

add_subdirectory(vendor)
add_subdirectory(src)

enable_testing()
add_subdirectory(tests)
