#include "core/TunerTarget.h"

#include <algorithm>
#include <cctype>

namespace fidget {
namespace {

bool HasNonWhitespace(const std::string& value)
{
    return std::any_of(
        value.begin(), value.end(), [](const unsigned char character) {
            return std::isspace(character) == 0;
        });
}

} // namespace

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
    const bool hostValid = HasNonWhitespace(input.mvlcHost)
        && input.mvlcCommandPort != 0U;
    const bool bridgeValid =
        input.endpointKind != TunerTargetEndpointKind::SshBridge
        || (HasNonWhitespace(input.sshDestination)
            && HasNonWhitespace(input.remoteBridgeCommand));
    result.endpointValid = hostValid && bridgeValid;
    if (!hostValid)
    {
        result.endpointMessage =
            "Enter an MVLC hostname or IP address.";
    }
    else if (!bridgeValid)
    {
        result.endpointMessage =
            "Enter an SSH destination and remote bridge command.";
    }

    const auto address = ParseTargetModuleAddress(input.moduleAddress);
    result.moduleAddressValid = address.success && address.address.has_value();
    result.moduleAddressMessage = address.message;
    result.normalizedModuleAddress = address.address;
    result.success = result.endpointValid && result.moduleAddressValid;
    return result;
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
