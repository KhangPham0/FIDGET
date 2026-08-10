add_library(fidget_cli_support STATIC
    CliApp.cpp
)

target_include_directories(fidget_cli_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

add_executable(fidget_cli main.cpp)
target_link_libraries(fidget_cli PRIVATE
    fidget_cli_support
    fidget_hardware
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(fidget_cli_support PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(fidget_cli PRIVATE -Wall -Wextra -Wpedantic)
endif()
