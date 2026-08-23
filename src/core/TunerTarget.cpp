#include "core/TunerTarget.h"

namespace fidget {

bool operator==(
    const TunerTargetInput& left,
    const TunerTargetInput& right) noexcept
{
    return left.endpointKind == right.endpointKind
        && left.mvlcHost == right.mvlcHost
        && left.mvlcCommandPort == right.mvlcCommandPort
        && left.moduleAddress == right.moduleAddress
        && left.sshDestination == right.sshDestination
        && left.remoteBridgeCommand == right.remoteBridgeCommand;
}

bool operator!=(
    const TunerTargetInput& left,
    const TunerTargetInput& right) noexcept
{
    return !(left == right);
}

bool TargetProbeEvidenceIsCurrent(
    const TunerTargetState& target) noexcept
{
    return !target.verification.invalidated
        && target.verification.probedInput.has_value()
        && *target.verification.probedInput == target.input
        && target.verification.result.outcome != TargetProbeOutcome::NotRun
        && target.verification.result.outcome != TargetProbeOutcome::InProgress;
}

bool TargetVerificationIsFresh(
    const TunerTargetState& target) noexcept
{
    return TargetProbeEvidenceIsCurrent(target)
        && target.verification.result.outcome
            == TargetProbeOutcome::VerifiedIdle;
}

void ApplyTargetPresentationEvidence(
    const TunerTargetState& target,
    TuningSessionEvidence& evidence) noexcept
{
    evidence.currentConnectionRequestValid =
        target.selection.has_value()
        && target.selection->input == target.input;
    evidence.controllerConnected = false;
    evidence.controllerIdentityVerified = false;
    evidence.targetIdentityAndFirmwareVerified = false;
    evidence.controllerIdleVerified = false;
    evidence.connectionVerificationFresh = false;
    evidence.activeControllerUseDetected = false;
    evidence.noControlTaken = false;
    evidence.noStateChangingCommandsSent = false;

    if (!TargetProbeEvidenceIsCurrent(target))
        return;

    const auto& probe = target.verification.result.evidence;
    evidence.controllerConnected = probe.controllerConnected;
    evidence.controllerIdentityVerified =
        probe.controllerIdentityAndFirmwareVerified;
    evidence.targetIdentityAndFirmwareVerified =
        probe.targetIdentityAndFirmwareVerified;
    evidence.controllerIdleVerified =
        probe.controllerDaqIdleVerified
        && probe.targetAcquisitionStoppedVerified;
    evidence.connectionVerificationFresh =
        TargetVerificationIsFresh(target);
    evidence.activeControllerUseDetected =
        probe.activeControllerUseDetected;
    evidence.noControlTaken = probe.noControlTaken;
    evidence.noStateChangingCommandsSent =
        probe.noStateChangingCommandsSent;
}

} // namespace fidget
