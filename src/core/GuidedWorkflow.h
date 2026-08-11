#ifndef FIDGET_CORE_GUIDED_WORKFLOW_H
#define FIDGET_CORE_GUIDED_WORKFLOW_H

#include <cstddef>

namespace fidget {

enum class GuidedTunerOwnershipState
{
    Disconnected,
    Checking,
    Idle,
    InUse,
    SessionOpen,
    OwnershipLost,
    RecoveryRequired,
    Failed,
};

enum class GuidedTunerAcquisitionState
{
    NotRun,
    Starting,
    Running,
    Stopping,
    Stopped,
    Failed,
};

enum class GuidedTunerOperation
{
    None,
    Audit,
    ConfigurationCapture,
    ProfileApplication,
    StartupPreparation,
    Other,
};

enum class GuidedTunerStage
{
    SelectProject,
    UnsupportedTarget,
    ResolveRecovery,
    CheckController,
    ConfirmHandoff,
    OpenSession,
    Working,
    LoadProfile,
    AuditStartup,
    CaptureConfiguration,
    ReviewStartup,
    Ready,
    Starting,
    Acquiring,
    Stopping,
    StoppedCleanly,
    Failed,
    Blocked,
};

enum class GuidedTunerAction
{
    None,
    ProjectControls,
    RecoveryControls,
    CheckControllerStatus,
    ConfirmHandoff,
    OpenSession,
    LoadProfile,
    RunStartupAudit,
    CaptureConfiguration,
    ReviewStartup,
    StartAcquisition,
    StopAcquisition,
};

enum class GuidedTunerTone
{
    Neutral,
    Working,
    Ready,
    Attention,
    Blocked,
};

struct GuidedTunerInputs
{
    bool projectActive = false;
    bool targetSupported = false;
    GuidedTunerOwnershipState ownership =
        GuidedTunerOwnershipState::Disconnected;
    bool mvmeHandoffConfirmed = false;
    bool recoveryRecordAvailable = false;
    GuidedTunerOperation activeOperation = GuidedTunerOperation::None;
    bool profileLoadedForTarget = false;
    bool startupAuditCompleteForTarget = false;
    bool startupAuditReady = false;
    bool configurationCompleteForTarget = false;
    bool configurationFresh = false;
    bool profileMatchesExactly = false;
    bool startupPlanAvailable = false;
    bool deterministicStartupPassed = false;
    GuidedTunerAcquisitionState acquisition =
        GuidedTunerAcquisitionState::NotRun;
    bool cleanupVerified = false;
};

struct GuidedTunerDecision
{
    GuidedTunerStage stage = GuidedTunerStage::SelectProject;
    GuidedTunerAction nextAction = GuidedTunerAction::ProjectControls;
    GuidedTunerTone tone = GuidedTunerTone::Neutral;
    std::size_t step = 1U;
    std::size_t totalSteps = 7U;
};

[[nodiscard]] GuidedTunerDecision PlanGuidedTunerWorkflow(
    const GuidedTunerInputs& inputs) noexcept;

[[nodiscard]] const char* GuidedTunerStageName(
    GuidedTunerStage stage) noexcept;
[[nodiscard]] const char* GuidedTunerStageMessage(
    GuidedTunerStage stage) noexcept;
[[nodiscard]] const char* GuidedTunerActionName(
    GuidedTunerAction action) noexcept;

} // namespace fidget

#endif
