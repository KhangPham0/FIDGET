add_executable(fidget_bridge main.cpp)
target_link_libraries(fidget_bridge PRIVATE fidget_hardware)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(fidget_bridge PRIVATE -Wall -Wextra -Wpedantic)
endif()
