#ifndef FIDGET_CORE_TUNER_SNAPSHOT_H
#define FIDGET_CORE_TUNER_SNAPSHOT_H

#include "core/CrateProject.h"
#include "core/GuidedWorkflow.h"
#include "core/StartupAudit.h"

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
    bool profileLoadedForTarget = false;
    bool startupAuditCompleteForTarget = false;
    bool startupAuditReady = false;
    StartupAuditResult startupAudit;
    bool configurationCompleteForTarget = false;
    bool configurationFresh = false;
    bool profileMatchesExactly = false;
    bool startupPlanAvailable = false;
    bool deterministicStartupPassed = false;
    GuidedTunerAcquisitionState acquisition =
        GuidedTunerAcquisitionState::NotRun;
    bool cleanupVerified = false;

    std::vector<TunerStatusMessage> statusMessages;
};

[[nodiscard]] GuidedTunerInputs MakeGuidedTunerInputs(
    const TunerSnapshot& snapshot) noexcept;

} // namespace fidget

#endif
