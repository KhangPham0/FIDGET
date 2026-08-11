#ifndef FIDGET_CORE_TUNER_SNAPSHOT_H
#define FIDGET_CORE_TUNER_SNAPSHOT_H

#include "core/Acquisition.h"
#include "core/CrateProject.h"
#include "core/DeterministicStartup.h"
#include "core/GuidedWorkflow.h"
#include "core/ScpConfiguration.h"
#include "core/ScpProfile.h"
#include "core/ScpTransactionPlan.h"
#include "core/ScpTransactionResult.h"
#include "core/StartupAudit.h"
#include "core/StartupPreparation.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fidget {

enum class TunerStatusLevel
{
    Information,
    Success,
    Warning,
    Error,
};

struct TunerStatusMessage
{
    TunerStatusLevel level = TunerStatusLevel::Information;
    std::string summary;
    std::string detail;
};

struct TunerSnapshot
{
    std::uint64_t revision = 0U;

    bool projectActive = false;
    std::string projectPath;
    std::string mvlcHost;
    std::uint16_t mvlcCommandPort = 32768U;
    std::size_t activeModuleIndex = 0U;
    std::string activeModuleName;
    std::uint32_t activeModuleBaseAddress = 0U;
    MdppBackend activeModuleBackend = MdppBackend::Scp;
    std::string activeModuleProfilePath;
    bool targetSupported = false;

    GuidedTunerOwnershipState ownership =
        GuidedTunerOwnershipState::Disconnected;
    bool mvmeHandoffConfirmed = false;
    bool controllerReadingsValid = false;
    std::uint32_t mvlcHardwareId = 0U;
    std::uint32_t mvlcFirmwareRevision = 0U;
    std::uint32_t mvlcDaqMode = 0U;

    bool recoveryRecordAvailable = false;
    GuidedTunerOperation activeOperation = GuidedTunerOperation::None;
    bool profileLoaded = false;
    bool profileLoadedForTarget = false;
    std::string loadedProfilePath;
    ScpProfile loadedProfile;
    bool startupAuditCompleteForTarget = false;
    bool startupAuditReady = false;
    StartupAuditResult startupAudit;
    bool configurationCompleteForTarget = false;
    bool configurationFresh = false;
    Fw2051ScpConfigurationSnapshot configurationCapture;
    ScpConfigurationComparison configurationComparison;
    ScpProfileApplicationPlan profileApplicationPlan;
    bool profileMatchesExactly = false;
    ScpSingleRepairResult singleRepairResult;
    ScpBulkApplyResult bulkApplyResult;
    bool startupPlanAvailable = false;
    ScpStandaloneStartupPlan standaloneStartupPlan;
    std::vector<Fw2051StartupPreparationMismatch>
        startupPreparationMismatches;
    bool deterministicStartupPassed = false;
    DeterministicStartupResult deterministicStartupResult;
    GuidedTunerAcquisitionState acquisition =
        GuidedTunerAcquisitionState::NotRun;
    DiagnosticAcquisitionResult diagnosticAcquisition;
    bool cleanupVerified = false;

    std::vector<TunerStatusMessage> statusMessages;
};

[[nodiscard]] GuidedTunerInputs MakeGuidedTunerInputs(
    const TunerSnapshot& snapshot) noexcept;

} // namespace fidget

#endif
