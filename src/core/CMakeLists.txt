add_library(fidget_core STATIC
    CrateProject.cpp
    GuidedWorkflow.cpp
    ReadoutProtocol.cpp
    RecoveryJournal.cpp
    ScpProfile.cpp
    ScpRegistry.cpp
    ScpTransactionPlan.cpp
    StreamDecoder.cpp
    TunerControl.cpp
    TunerSnapshot.cpp
    VmeProtocol.cpp
)

target_include_directories(fidget_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(fidget_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
