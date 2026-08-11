find_package(Threads REQUIRED)

add_library(fidget_hardware STATIC
    CommandWorker.cpp
    MvlcCommandTransport.cpp
    MvlcDataReceiver.cpp
    OwnershipService.cpp
    ScpCaptureOperation.cpp
    Transport.cpp
    VmeTransaction.cpp
)

target_include_directories(fidget_hardware PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

target_link_libraries(fidget_hardware PUBLIC
    fidget_core
    Threads::Threads
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(fidget_hardware PRIVATE -Wall -Wextra -Wpedantic)
endif()
