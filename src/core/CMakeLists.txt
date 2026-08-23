add_library(fidget_core STATIC
    ActivityLog.cpp
    ActivityLogFile.cpp
    ApplicationStorage.cpp
    BridgeProtocol.cpp
    CrateProject.cpp
    GuidedWorkflow.cpp
    MvmeExport.cpp
    MvmeExportFile.cpp
    ReadoutProtocol.cpp
    RecoveryJournal.cpp
    RecoveryVerification.cpp
    ScpProfile.cpp
    ScpRegistry.cpp
    ScpTransactionPlan.cpp
    StartupAudit.cpp
    StartupPreparation.cpp
    StreamDecoder.cpp
    TunerControl.cpp
    TunerSnapshot.cpp
    VmeProtocol.cpp
)

target_include_directories(fidget_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)

# GCC 8 ships C++17 filesystem in a separate standard-library archive.
# Attaching it here as a usage requirement keeps the link order correct
# for every consumer that pulls filesystem-using objects out of this
# archive. It is part of the standard library, not a project dependency,
# so the rule that fidget_core links no project library still holds.
if(FIDGET_CXX17_FILESYSTEM_LIBRARY)
    target_link_libraries(fidget_core PUBLIC ${FIDGET_CXX17_FILESYSTEM_LIBRARY})
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(fidget_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
