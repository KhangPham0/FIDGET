cmake_minimum_required(VERSION 3.24)
project(fidget VERSION 0.1.0 LANGUAGES C CXX)

# C++17: the oldest toolchains this tool must build on stop there.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# So editors and clang tooling can find the vendored headers.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_subdirectory(vendor)
add_subdirectory(src)
