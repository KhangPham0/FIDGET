add_library(fidget_core STATIC
    StreamDecoder.cpp
    VmeProtocol.cpp
)

target_include_directories(fidget_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(fidget_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
