add_library(fidget_core STATIC
    ActivityLog.cpp
    ActivityLogFile.cpp
    ApplicationRecoveryDiscovery.cpp
    ApplicationStorage.cpp
    BridgeProtocol.cpp
    ControllerEndpoint.cpp
    CrateProject.cpp
    GuidedWorkflow.cpp
    MvmeInitScript.cpp
    MvmeExport.cpp
    MvmeExportFile.cpp
    MvmeWorkspace.cpp
    MvmeWorkspaceExport.cpp
    ReadoutProtocol.cpp
    RecoveryJournal.cpp
    RecoveryJournalV5.cpp
    RecoveryVerification.cpp
    ScpProfile.cpp
    ScpRegistry.cpp
    ScpTransactionPlan.cpp
    StartupAudit.cpp
    StartupPreparation.cpp
    StreamDecoder.cpp
    TargetModuleAddress.cpp
    TunerControl.cpp
    TunerSnapshot.cpp
    TunerTarget.cpp
    VmeProtocol.cpp
)

target_include_directories(fidget_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(fidget_core PUBLIC nlohmann_json::nlohmann_json)

# GCC 8 ships C++17 filesystem in a separate standard-library archive.
# Attaching it here as a usage requirement keeps the link order correct
# for every consumer that pulls filesystem-using objects out of this
# archive. It is part of the standard library rather than a vendored
# runtime dependency.
if(FIDGET_CXX17_FILESYSTEM_LIBRARY)
    target_link_libraries(fidget_core PUBLIC ${FIDGET_CXX17_FILESYSTEM_LIBRARY})
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(fidget_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
