#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/TunerControl.h"
#include "core/TunerSnapshot.h"

#include <variant>

namespace {

fidget::TunerTargetInput TargetInput()
{
    fidget::TunerTargetInput input;
    input.mvlcHost = "mvlc-test";
    input.moduleAddress = "0x1100";
    input.sshDestination = "bridge-test";
    input.remoteBridgeCommand = "/opt/fidget_bridge";
    return input;
}

fidget::TunerTargetState VerifiedTarget()
{
    using namespace fidget;

    auto target = TunerTargetState{};
    target.input = TargetInput();
    const auto address = ParseTargetModuleAddress(target.input.moduleAddress);
    target.selection = TunerTargetSelection{
        target.input,
        address.address.value(),
    };
    target.verification.probedInput = target.input;
    target.verification.result.outcome = TargetProbeOutcome::VerifiedIdle;
    auto& evidence = target.verification.result.evidence;
    evidence.controllerConnected = true;
    evidence.controllerIdentityAndFirmwareVerified = true;
    evidence.controllerDaqIdleVerified = true;
    evidence.targetIdentityAndFirmwareVerified = true;
    evidence.targetAcquisitionStoppedVerified = true;
    evidence.noControlTaken = true;
    evidence.noStateChangingCommandsSent = true;
    return target;
}

} // namespace

TEST_CASE("a tuner snapshot supplies every guided workflow input")
{
    using namespace fidget;

    TunerSnapshot snapshot;
    snapshot.projectActive = true;
    snapshot.targetSupported = true;
    snapshot.ownership = GuidedTunerOwnershipState::SessionOpen;
    snapshot.mvmeHandoffConfirmed = true;
    snapshot.recoveryRecordAvailable = true;
    snapshot.activeOperation = GuidedTunerOperation::StartupPreparation;
    snapshot.profileLoadedForTarget = true;
    snapshot.startupAuditCompleteForTarget = true;
    snapshot.startupAuditReady = true;
    snapshot.configurationCompleteForTarget = true;
    snapshot.configurationFresh = true;
    snapshot.profileMatchesExactly = true;
    snapshot.startupPlanAvailable = true;
    snapshot.deterministicStartupPassed = true;
    snapshot.acquisition = GuidedTunerAcquisitionState::Running;
    snapshot.cleanupVerified = true;

    const auto inputs = MakeGuidedTunerInputs(snapshot);
    CHECK(inputs.projectActive);
    CHECK(inputs.targetSupported);
    CHECK(inputs.ownership == GuidedTunerOwnershipState::SessionOpen);
    CHECK(inputs.mvmeHandoffConfirmed);
    CHECK(inputs.recoveryRecordAvailable);
    CHECK(inputs.activeOperation == GuidedTunerOperation::StartupPreparation);
    CHECK(inputs.profileLoadedForTarget);
    CHECK(inputs.startupAuditCompleteForTarget);
    CHECK(inputs.startupAuditReady);
    CHECK(inputs.configurationCompleteForTarget);
    CHECK(inputs.configurationFresh);
    CHECK(inputs.profileMatchesExactly);
    CHECK(inputs.startupPlanAvailable);
    CHECK(inputs.deterministicStartupPassed);
    CHECK(inputs.acquisition == GuidedTunerAcquisitionState::Running);
    CHECK(inputs.cleanupVerified);
}

TEST_CASE("ownership commands remain explicit semantic types")
{
    using namespace fidget;

    TunerCommand command = CheckStatusCommand{};
    CHECK(std::holds_alternative<CheckStatusCommand>(command));

    command = SetMvmeHandoffConfirmedCommand{true};
    REQUIRE(std::holds_alternative<SetMvmeHandoffConfirmedCommand>(command));
    CHECK(std::get<SetMvmeHandoffConfirmedCommand>(command).confirmed);

    command = RunStartupAuditCommand{};
    CHECK(std::holds_alternative<RunStartupAuditCommand>(command));

    command = CaptureConfigurationCommand{};
    CHECK(std::holds_alternative<CaptureConfigurationCommand>(command));

    command = SaveProfileCommand{"saved.mwwscp"};
    REQUIRE(std::holds_alternative<SaveProfileCommand>(command));
    CHECK(std::get<SaveProfileCommand>(command).path == "saved.mwwscp");

    command = LoadProfileCommand{"loaded.mwwscp"};
    REQUIRE(std::holds_alternative<LoadProfileCommand>(command));
    CHECK(std::get<LoadProfileCommand>(command).path == "loaded.mwwscp");

    command = RunDeterministicStartupCommand{true};
    REQUIRE(std::holds_alternative<RunDeterministicStartupCommand>(command));
    CHECK(std::get<RunDeterministicStartupCommand>(command).confirmed);

    UseCrateProjectCommand useProject;
    useProject.project.mvlcHost = "mvlc-test";
    command = useProject;
    REQUIRE(std::holds_alternative<UseCrateProjectCommand>(command));
    CHECK(std::get<UseCrateProjectCommand>(command).project.mvlcHost
          == "mvlc-test");
}

TEST_CASE("project-independent target commands remain explicit semantic types")
{
    using namespace fidget;

    auto input = TargetInput();
    input.endpointKind = TunerTargetEndpointKind::SshBridge;

    TunerCommand command = EditTunerTargetCommand{input};
    REQUIRE(std::holds_alternative<EditTunerTargetCommand>(command));
    CHECK(std::get<EditTunerTargetCommand>(command).input == input);

    command = SelectTunerTargetCommand{};
    CHECK(std::holds_alternative<SelectTunerTargetCommand>(command));
    command = ProbeTunerTargetCommand{};
    CHECK(std::holds_alternative<ProbeTunerTargetCommand>(command));
    command = OpenTunerTargetSessionCommand{};
    CHECK(std::holds_alternative<OpenTunerTargetSessionCommand>(command));
    command = ClearTunerTargetCommand{};
    CHECK(std::holds_alternative<ClearTunerTargetCommand>(command));
}

TEST_CASE("verified target facts feed the GUI evidence model")
{
    using namespace fidget;

    TunerSnapshot snapshot;
    snapshot.target = VerifiedTarget();
    ApplyTargetPresentationEvidence(
        snapshot.target,
        snapshot.tuningSession.evidence);

    const auto& evidence = snapshot.tuningSession.evidence;
    CHECK(evidence.currentConnectionRequestValid);
    CHECK(evidence.controllerConnected);
    CHECK(evidence.controllerIdentityVerified);
    CHECK(evidence.targetIdentityAndFirmwareVerified);
    CHECK(evidence.controllerIdleVerified);
    CHECK(evidence.connectionVerificationFresh);
    CHECK(evidence.noControlTaken);
    CHECK(evidence.noStateChangingCommandsSent);
    CHECK_FALSE(evidence.activeControllerUseDetected);
}

TEST_CASE("every editable target connection field invalidates old evidence")
{
    using namespace fidget;

    const auto checkInvalidated = [](
        const TunerTargetState& target,
        const bool requestStillValid = false) {
        TuningSessionEvidence evidence;
        evidence.controllerConnected = true;
        evidence.controllerIdentityVerified = true;
        evidence.targetIdentityAndFirmwareVerified = true;
        evidence.controllerIdleVerified = true;
        evidence.connectionVerificationFresh = true;
        ApplyTargetPresentationEvidence(target, evidence);
        CHECK_FALSE(TargetProbeEvidenceIsCurrent(target));
        CHECK_FALSE(TargetVerificationIsFresh(target));
        CHECK(evidence.currentConnectionRequestValid == requestStillValid);
        CHECK_FALSE(evidence.controllerConnected);
        CHECK_FALSE(evidence.controllerIdentityVerified);
        CHECK_FALSE(evidence.targetIdentityAndFirmwareVerified);
        CHECK_FALSE(evidence.controllerIdleVerified);
        CHECK_FALSE(evidence.connectionVerificationFresh);
    };

    auto target = VerifiedTarget();
    target.input.mvlcHost = "another-controller";
    checkInvalidated(target);

    target = VerifiedTarget();
    target.input.mvlcCommandPort = 40000U;
    checkInvalidated(target);

    target = VerifiedTarget();
    target.input.moduleAddress = "0x2200";
    checkInvalidated(target);

    target = VerifiedTarget();
    target.input.endpointKind = TunerTargetEndpointKind::SshBridge;
    checkInvalidated(target);

    target = VerifiedTarget();
    target.input.sshDestination = "another-bridge";
    checkInvalidated(target);

    target = VerifiedTarget();
    target.input.remoteBridgeCommand = "/srv/fidget_bridge";
    checkInvalidated(target);

    target = VerifiedTarget();
    target.verification.invalidated = true;
    checkInvalidated(target, true);
}

TEST_CASE("active-use target evidence routes into the presentation hazard facts")
{
    using namespace fidget;

    auto target = VerifiedTarget();
    target.verification.result.outcome =
        TargetProbeOutcome::ControllerDaqActive;
    target.verification.result.evidence.controllerDaqIdleVerified = false;
    target.verification.result.evidence
        .targetIdentityAndFirmwareVerified = false;
    target.verification.result.evidence
        .targetAcquisitionStoppedVerified = false;
    target.verification.result.evidence.activeControllerUseDetected = true;

    TuningSessionEvidence evidence;
    ApplyTargetPresentationEvidence(target, evidence);
    CHECK(TargetProbeEvidenceIsCurrent(target));
    CHECK_FALSE(TargetVerificationIsFresh(target));
    CHECK(evidence.activeControllerUseDetected);
    CHECK(evidence.noControlTaken);
    CHECK(evidence.noStateChangingCommandsSent);
    CHECK_FALSE(evidence.connectionVerificationFresh);
}
