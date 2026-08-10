#include "core/GuidedWorkflow.h"

namespace fidget {
namespace {

GuidedTunerDecision MakeDecision(
    const GuidedTunerStage stage,
    const GuidedTunerAction action,
    const GuidedTunerTone tone,
    const std::size_t step) noexcept
{
    return {stage, action, tone, step, 7U};
}

} // namespace

GuidedTunerDecision PlanGuidedTunerWorkflow(
    const GuidedTunerInputs& inputs) noexcept
{
    if (inputs.recoveryRecordAvailable
        || inputs.ownership == GuidedTunerOwnershipState::RecoveryRequired)
    {
        return MakeDecision(
            GuidedTunerStage::ResolveRecovery,
            GuidedTunerAction::RecoveryControls,
            GuidedTunerTone::Blocked, 2U);
    }

    switch (inputs.acquisition)
    {
    case GuidedTunerAcquisitionState::Starting:
        return MakeDecision(
            GuidedTunerStage::Starting, GuidedTunerAction::None,
            GuidedTunerTone::Working, 7U);
    case GuidedTunerAcquisitionState::Running:
        return MakeDecision(
            GuidedTunerStage::Acquiring,
            GuidedTunerAction::StopAcquisition,
            GuidedTunerTone::Ready, 7U);
    case GuidedTunerAcquisitionState::Stopping:
        return MakeDecision(
            GuidedTunerStage::Stopping, GuidedTunerAction::None,
            GuidedTunerTone::Working, 7U);
    case GuidedTunerAcquisitionState::Stopped:
        if (inputs.cleanupVerified
            && inputs.ownership == GuidedTunerOwnershipState::SessionOpen)
        {
            return MakeDecision(
                GuidedTunerStage::StoppedCleanly,
                GuidedTunerAction::StartAcquisition,
                GuidedTunerTone::Ready, 7U);
        }
        if (!inputs.cleanupVerified)
        {
            return MakeDecision(
                GuidedTunerStage::Failed, GuidedTunerAction::None,
                GuidedTunerTone::Blocked, 7U);
        }
        break;
    case GuidedTunerAcquisitionState::Failed:
        return MakeDecision(
            GuidedTunerStage::Failed, GuidedTunerAction::None,
            GuidedTunerTone::Blocked, 7U);
    case GuidedTunerAcquisitionState::NotRun:
        break;
    }

    if (!inputs.projectActive)
    {
        return MakeDecision(
            GuidedTunerStage::SelectProject,
            GuidedTunerAction::ProjectControls,
            GuidedTunerTone::Attention, 1U);
    }

    if (!inputs.targetSupported)
    {
        return MakeDecision(
            GuidedTunerStage::UnsupportedTarget,
            GuidedTunerAction::ProjectControls,
            GuidedTunerTone::Blocked, 1U);
    }

    switch (inputs.ownership)
    {
    case GuidedTunerOwnershipState::Disconnected:
        return MakeDecision(
            GuidedTunerStage::CheckController,
            GuidedTunerAction::CheckControllerStatus,
            GuidedTunerTone::Attention, 2U);
    case GuidedTunerOwnershipState::Checking:
        return MakeDecision(
            GuidedTunerStage::Working, GuidedTunerAction::None,
            GuidedTunerTone::Working, 2U);
    case GuidedTunerOwnershipState::Idle:
        if (!inputs.mvmeHandoffConfirmed)
        {
            return MakeDecision(
                GuidedTunerStage::ConfirmHandoff,
                GuidedTunerAction::ConfirmHandoff,
                GuidedTunerTone::Attention, 2U);
        }
        return MakeDecision(
            GuidedTunerStage::OpenSession,
            GuidedTunerAction::OpenSession,
            GuidedTunerTone::Attention, 2U);
    case GuidedTunerOwnershipState::InUse:
    case GuidedTunerOwnershipState::OwnershipLost:
        return MakeDecision(
            GuidedTunerStage::Blocked,
            GuidedTunerAction::CheckControllerStatus,
            GuidedTunerTone::Blocked, 2U);
    case GuidedTunerOwnershipState::Failed:
        return MakeDecision(
            GuidedTunerStage::CheckController,
            GuidedTunerAction::CheckControllerStatus,
            GuidedTunerTone::Blocked, 2U);
    case GuidedTunerOwnershipState::RecoveryRequired:
        return MakeDecision(
            GuidedTunerStage::ResolveRecovery,
            GuidedTunerAction::RecoveryControls,
            GuidedTunerTone::Blocked, 2U);
    case GuidedTunerOwnershipState::SessionOpen:
        break;
    }

    if (inputs.activeOperation != GuidedTunerOperation::None)
    {
        std::size_t step = 2U;
        if (inputs.activeOperation == GuidedTunerOperation::Audit)
        {
            step = 4U;
        }
        else if (inputs.activeOperation
                 == GuidedTunerOperation::ConfigurationCapture)
        {
            step = 5U;
        }
        else if (inputs.activeOperation
                 == GuidedTunerOperation::StartupPreparation)
        {
            step = 6U;
        }
        return MakeDecision(
            GuidedTunerStage::Working, GuidedTunerAction::None,
            GuidedTunerTone::Working, step);
    }

    if (!inputs.profileLoadedForTarget)
    {
        return MakeDecision(
            GuidedTunerStage::LoadProfile,
            GuidedTunerAction::LoadProfile,
            GuidedTunerTone::Attention, 3U);
    }

    if (!inputs.startupAuditCompleteForTarget)
    {
        return MakeDecision(
            GuidedTunerStage::AuditStartup,
            GuidedTunerAction::RunStartupAudit,
            GuidedTunerTone::Attention, 4U);
    }

    if (!inputs.configurationCompleteForTarget
        || !inputs.configurationFresh)
    {
        return MakeDecision(
            GuidedTunerStage::CaptureConfiguration,
            GuidedTunerAction::CaptureConfiguration,
            GuidedTunerTone::Attention, 5U);
    }

    if (inputs.deterministicStartupPassed
        || (inputs.startupAuditReady && inputs.profileMatchesExactly))
    {
        return MakeDecision(
            GuidedTunerStage::Ready,
            GuidedTunerAction::StartAcquisition,
            GuidedTunerTone::Ready, 7U);
    }

    if (inputs.startupPlanAvailable)
    {
        return MakeDecision(
            GuidedTunerStage::ReviewStartup,
            GuidedTunerAction::ReviewStartup,
            GuidedTunerTone::Attention, 6U);
    }

    return MakeDecision(
        GuidedTunerStage::Blocked, GuidedTunerAction::None,
        GuidedTunerTone::Blocked, 6U);
}

const char* GuidedTunerStageName(const GuidedTunerStage stage) noexcept
{
    switch (stage)
    {
    case GuidedTunerStage::SelectProject:
        return "Select a crate project";
    case GuidedTunerStage::UnsupportedTarget:
        return "Select a supported module";
    case GuidedTunerStage::ResolveRecovery:
        return "Recovery required";
    case GuidedTunerStage::CheckController:
        return "Check the MVLC";
    case GuidedTunerStage::ConfirmHandoff:
        return "Confirm the MVME handoff";
    case GuidedTunerStage::OpenSession:
        return "Open the tuner session";
    case GuidedTunerStage::Working:
        return "Working";
    case GuidedTunerStage::LoadProfile:
        return "Load the saved SCP profile";
    case GuidedTunerStage::AuditStartup:
        return "Audit the module startup state";
    case GuidedTunerStage::CaptureConfiguration:
        return "Read the live SCP configuration";
    case GuidedTunerStage::ReviewStartup:
        return "Review deterministic startup";
    case GuidedTunerStage::Ready:
        return "Ready to acquire waveforms";
    case GuidedTunerStage::Starting:
        return "Starting acquisition";
    case GuidedTunerStage::Acquiring:
        return "Acquiring waveforms";
    case GuidedTunerStage::Stopping:
        return "Stopping safely";
    case GuidedTunerStage::StoppedCleanly:
        return "Stopped cleanly";
    case GuidedTunerStage::Failed:
        return "Action failed";
    case GuidedTunerStage::Blocked:
        return "Blocked";
    }
    return "Unknown";
}

const char* GuidedTunerStageMessage(const GuidedTunerStage stage) noexcept
{
    switch (stage)
    {
    case GuidedTunerStage::SelectProject:
        return "Load or activate the crate project that describes this MVLC, its modules, and profile paths.";
    case GuidedTunerStage::UnsupportedTarget:
        return "The selected project module is not supported by the current SCP tuner backend.";
    case GuidedTunerStage::ResolveRecovery:
        return "Resolve the recorded tuner-owned state before making any new hardware request.";
    case GuidedTunerStage::CheckController:
        return "Run the read-only controller check. It closes its connection immediately and performs no VME write.";
    case GuidedTunerStage::ConfirmHandoff:
        return "Confirm that the MVME run is stopped and MVME is completely quit before the tuner retains ownership.";
    case GuidedTunerStage::OpenSession:
        return "The MVLC is idle and the handoff is confirmed. Open the exclusive tuner session.";
    case GuidedTunerStage::Working:
        return "A previously tested operation is in progress. Wait for its verified result.";
    case GuidedTunerStage::LoadProfile:
        return "Load the saved profile for the selected module so live hardware can be checked against the intended settings.";
    case GuidedTunerStage::AuditStartup:
        return "Run the zero-write 37-register startup audit for the selected module.";
    case GuidedTunerStage::CaptureConfiguration:
        return "Capture a fresh eight-quad SCP snapshot for exact comparison with the loaded profile.";
    case GuidedTunerStage::ReviewStartup:
        return "The live state is not yet ready. Review and explicitly approve the existing rollback-protected startup recipe.";
    case GuidedTunerStage::Ready:
        return "The selected module satisfies the readout contract and saved configuration. Acquisition remains a separate user action.";
    case GuidedTunerStage::Starting:
        return "The tuner is isolating non-target modules and installing the verified diagnostic readout path.";
    case GuidedTunerStage::Acquiring:
        return "Waveforms are live. Stop uses the verified cleanup path and checks every isolated module.";
    case GuidedTunerStage::Stopping:
        return "Verified cleanup is in progress. Do not start MVME until it completes.";
    case GuidedTunerStage::StoppedCleanly:
        return "The selected module, isolated modules, MVLC DAQ mode, and diagnostic stack were cleaned up successfully.";
    case GuidedTunerStage::Failed:
        return "The last acquisition action did not finish with verified cleanup. Review the detailed status before continuing.";
    case GuidedTunerStage::Blocked:
        return "A safety prerequisite is not satisfied. Review the detailed ownership or configuration message before continuing.";
    }
    return "Unknown workflow state.";
}

const char* GuidedTunerActionName(const GuidedTunerAction action) noexcept
{
    switch (action)
    {
    case GuidedTunerAction::None:
        return "";
    case GuidedTunerAction::ProjectControls:
        return "crate project";
    case GuidedTunerAction::RecoveryControls:
        return "recovery controls";
    case GuidedTunerAction::CheckControllerStatus:
        return "MVLC status check";
    case GuidedTunerAction::ConfirmHandoff:
        return "MVME handoff confirmation";
    case GuidedTunerAction::OpenSession:
        return "open-session control";
    case GuidedTunerAction::LoadProfile:
        return "profile loader";
    case GuidedTunerAction::RunStartupAudit:
        return "startup audit";
    case GuidedTunerAction::CaptureConfiguration:
        return "configuration capture";
    case GuidedTunerAction::ReviewStartup:
        return "startup review";
    case GuidedTunerAction::StartAcquisition:
        return "acquisition control";
    case GuidedTunerAction::StopAcquisition:
        return "stop control";
    }
    return "";
}

} // namespace fidget
