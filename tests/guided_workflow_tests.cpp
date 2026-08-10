#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/GuidedWorkflow.h"

namespace {

void CheckDecision(
    const fidget::GuidedTunerInputs& inputs,
    const fidget::GuidedTunerStage stage,
    const fidget::GuidedTunerAction action,
    const fidget::GuidedTunerTone tone,
    const std::size_t step)
{
    const auto result = fidget::PlanGuidedTunerWorkflow(inputs);
    CHECK(result.stage == stage);
    CHECK(result.nextAction == action);
    CHECK(result.tone == tone);
    CHECK(result.step == step);
    CHECK(result.totalSteps == 7U);
}

fidget::GuidedTunerInputs MakeSessionInputs()
{
    fidget::GuidedTunerInputs inputs;
    inputs.projectActive = true;
    inputs.targetSupported = true;
    inputs.ownership = fidget::GuidedTunerOwnershipState::SessionOpen;
    return inputs;
}

} // namespace

TEST_CASE("the guided workflow preserves the project and ownership path")
{
    using namespace fidget;

    GuidedTunerInputs inputs;
    CheckDecision(
        inputs, GuidedTunerStage::SelectProject,
        GuidedTunerAction::ProjectControls,
        GuidedTunerTone::Attention, 1U);

    inputs.projectActive = true;
    CheckDecision(
        inputs, GuidedTunerStage::UnsupportedTarget,
        GuidedTunerAction::ProjectControls,
        GuidedTunerTone::Blocked, 1U);

    inputs.targetSupported = true;
    CheckDecision(
        inputs, GuidedTunerStage::CheckController,
        GuidedTunerAction::CheckControllerStatus,
        GuidedTunerTone::Attention, 2U);

    inputs.ownership = GuidedTunerOwnershipState::Idle;
    CheckDecision(
        inputs, GuidedTunerStage::ConfirmHandoff,
        GuidedTunerAction::ConfirmHandoff,
        GuidedTunerTone::Attention, 2U);

    inputs.mvmeHandoffConfirmed = true;
    CheckDecision(
        inputs, GuidedTunerStage::OpenSession,
        GuidedTunerAction::OpenSession,
        GuidedTunerTone::Attention, 2U);
}

TEST_CASE("recovery always takes priority in the guided workflow")
{
    using namespace fidget;

    GuidedTunerInputs inputs;
    inputs.recoveryRecordAvailable = true;
    CheckDecision(
        inputs, GuidedTunerStage::ResolveRecovery,
        GuidedTunerAction::RecoveryControls,
        GuidedTunerTone::Blocked, 2U);

    inputs.recoveryRecordAvailable = false;
    inputs.ownership = GuidedTunerOwnershipState::RecoveryRequired;
    CheckDecision(
        inputs, GuidedTunerStage::ResolveRecovery,
        GuidedTunerAction::RecoveryControls,
        GuidedTunerTone::Blocked, 2U);
}

TEST_CASE("the guided workflow preserves the readiness path")
{
    using namespace fidget;

    auto inputs = MakeSessionInputs();
    CheckDecision(
        inputs, GuidedTunerStage::LoadProfile,
        GuidedTunerAction::LoadProfile,
        GuidedTunerTone::Attention, 3U);

    inputs.profileLoadedForTarget = true;
    CheckDecision(
        inputs, GuidedTunerStage::AuditStartup,
        GuidedTunerAction::RunStartupAudit,
        GuidedTunerTone::Attention, 4U);

    inputs.startupAuditCompleteForTarget = true;
    inputs.startupAuditReady = true;
    CheckDecision(
        inputs, GuidedTunerStage::CaptureConfiguration,
        GuidedTunerAction::CaptureConfiguration,
        GuidedTunerTone::Attention, 5U);

    inputs.configurationCompleteForTarget = true;
    inputs.configurationFresh = true;
    inputs.profileMatchesExactly = true;
    CheckDecision(
        inputs, GuidedTunerStage::Ready,
        GuidedTunerAction::StartAcquisition,
        GuidedTunerTone::Ready, 7U);
}

TEST_CASE("the guided workflow preserves preparation and busy states")
{
    using namespace fidget;

    auto inputs = MakeSessionInputs();
    inputs.profileLoadedForTarget = true;
    inputs.startupAuditCompleteForTarget = true;
    inputs.configurationCompleteForTarget = true;
    inputs.configurationFresh = true;
    inputs.startupPlanAvailable = true;
    CheckDecision(
        inputs, GuidedTunerStage::ReviewStartup,
        GuidedTunerAction::ReviewStartup,
        GuidedTunerTone::Attention, 6U);

    inputs.activeOperation = GuidedTunerOperation::StartupPreparation;
    CheckDecision(
        inputs, GuidedTunerStage::Working,
        GuidedTunerAction::None,
        GuidedTunerTone::Working, 6U);

    inputs.activeOperation = GuidedTunerOperation::None;
    inputs.deterministicStartupPassed = true;
    CheckDecision(
        inputs, GuidedTunerStage::Ready,
        GuidedTunerAction::StartAcquisition,
        GuidedTunerTone::Ready, 7U);
}

TEST_CASE("the guided workflow preserves acquisition and cleanup states")
{
    using namespace fidget;

    auto inputs = MakeSessionInputs();
    inputs.acquisition = GuidedTunerAcquisitionState::Running;
    CheckDecision(
        inputs, GuidedTunerStage::Acquiring,
        GuidedTunerAction::StopAcquisition,
        GuidedTunerTone::Ready, 7U);

    inputs.acquisition = GuidedTunerAcquisitionState::Stopping;
    CheckDecision(
        inputs, GuidedTunerStage::Stopping,
        GuidedTunerAction::None,
        GuidedTunerTone::Working, 7U);

    inputs.acquisition = GuidedTunerAcquisitionState::Stopped;
    inputs.cleanupVerified = true;
    CheckDecision(
        inputs, GuidedTunerStage::StoppedCleanly,
        GuidedTunerAction::StartAcquisition,
        GuidedTunerTone::Ready, 7U);

    inputs.cleanupVerified = false;
    CheckDecision(
        inputs, GuidedTunerStage::Failed,
        GuidedTunerAction::None,
        GuidedTunerTone::Blocked, 7U);

    inputs.cleanupVerified = true;
    inputs.ownership = GuidedTunerOwnershipState::Disconnected;
    CheckDecision(
        inputs, GuidedTunerStage::CheckController,
        GuidedTunerAction::CheckControllerStatus,
        GuidedTunerTone::Attention, 2U);
}
