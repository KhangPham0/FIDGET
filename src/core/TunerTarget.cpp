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

TunerTargetInputValidation ValidateTunerTargetInput(
    const TunerTargetInput& input)
{
    TunerTargetInputValidation result;
    const auto endpoint = ValidateControllerEndpoint({
        input.endpointKind,
        input.mvlcHost,
        input.mvlcCommandPort,
        input.sshDestination,
        input.remoteBridgeCommand,
    });
    result.endpointValid = endpoint.success;
    result.endpointMessage = endpoint.message;

    const auto address = ParseTargetModuleAddress(input.moduleAddress);
    result.moduleAddressValid = address.success && address.address.has_value();
    result.moduleAddressMessage = address.message;
    result.normalizedModuleAddress = address.address;
    result.success = result.endpointValid && result.moduleAddressValid;
    return result;
}

ControllerEndpointRequest ControllerEndpointForTarget(
    const TunerTargetInput& input)
{
    return {
        input.endpointKind,
        input.mvlcHost,
        input.mvlcCommandPort,
        input.sshDestination,
        input.remoteBridgeCommand,
    };
}

bool ControllerProbeEvidenceIsCurrent(
    const TunerTargetState& target) noexcept
{
    if (!target.controllerVerification.probedEndpoint)
        return false;

    const auto& endpoint = *target.controllerVerification.probedEndpoint;
    const auto& input = target.input;
    return !target.controllerVerification.invalidated
        && endpoint.kind == input.endpointKind
        && endpoint.mvlcHost == input.mvlcHost
        && endpoint.mvlcCommandPort == input.mvlcCommandPort
        && endpoint.sshDestination == input.sshDestination
        && endpoint.remoteBridgeCommand == input.remoteBridgeCommand
        && target.controllerVerification.result.outcome
            != ControllerProbeOutcome::NotRun
        && target.controllerVerification.result.outcome
            != ControllerProbeOutcome::InProgress;
}

bool ControllerVerificationIsFresh(
    const TunerTargetState& target) noexcept
{
    return ControllerProbeEvidenceIsCurrent(target)
        && target.controllerVerification.result.outcome
            == ControllerProbeOutcome::VerifiedIdle;
}

bool TargetProbeEvidenceIsCurrent(
    const TunerTargetState& target) noexcept
{
    return ControllerVerificationIsFresh(target)
        && !target.verification.invalidated
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
        ControllerProbeEvidenceIsCurrent(target);
    evidence.controllerConnected = false;
    evidence.controllerIdentityVerified = false;
    evidence.controllerVerificationFresh = false;
    evidence.targetIdentityAndFirmwareVerified = false;
    evidence.controllerIdleVerified = false;
    evidence.targetAcquisitionStoppedVerified = false;
    evidence.targetVerificationFresh = false;
    evidence.connectionVerificationFresh = false;
    evidence.activeControllerUseDetected = false;
    evidence.noControlTaken = false;
    evidence.noVmeOrModuleSettingWritesSent = false;

    if (ControllerProbeEvidenceIsCurrent(target))
    {
        const auto& controller =
            target.controllerVerification.result.evidence;
        evidence.controllerConnected = controller.controllerConnected;
        evidence.controllerIdentityVerified =
            controller.controllerIdentityAndFirmwareVerified;
        evidence.controllerIdleVerified =
            controller.controllerDaqIdleVerified;
        evidence.controllerVerificationFresh =
            ControllerVerificationIsFresh(target);
        evidence.activeControllerUseDetected =
            controller.activeControllerUseDetected;
        evidence.noControlTaken = controller.noControlTaken;
        evidence.noVmeOrModuleSettingWritesSent =
            controller.noVmeOrModuleSettingWritesSent;
    }

    if (!TargetProbeEvidenceIsCurrent(target))
        return;

    const auto& probe = target.verification.result.evidence;
    evidence.targetIdentityAndFirmwareVerified =
        probe.targetIdentityAndFirmwareVerified;
    evidence.targetAcquisitionStoppedVerified =
        probe.targetAcquisitionStoppedVerified;
    evidence.targetVerificationFresh =
        TargetVerificationIsFresh(target);
    evidence.connectionVerificationFresh =
        TargetVerificationIsFresh(target);
    evidence.activeControllerUseDetected =
        evidence.activeControllerUseDetected
        || probe.activeControllerUseDetected;
    evidence.noControlTaken =
        evidence.noControlTaken && probe.noControlTaken;
    evidence.noVmeOrModuleSettingWritesSent =
        evidence.noVmeOrModuleSettingWritesSent
        && probe.noVmeOrModuleSettingWritesSent;
}

} // namespace fidget
