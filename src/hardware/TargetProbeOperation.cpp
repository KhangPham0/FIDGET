#include "hardware/TargetProbeOperation.h"

#include "hardware/VmeTransaction.h"

#include <string>
#include <utility>
#include <variant>

namespace fidget {
namespace {

constexpr const char ActiveUseMessage[] =
    "Active controller use was detected. FIDGET did not take control, and "
    "no hardware settings were changed.";

const std::string& EndpointHost(const TransportEndpointRequest& request)
{
    return std::visit(
        [](const auto& endpoint) -> const std::string& {
            return endpoint.mvlcHost;
        },
        request);
}

std::uint16_t EndpointPort(const TransportEndpointRequest& request)
{
    return std::visit(
        [](const auto& endpoint) {
            return endpoint.mvlcCommandPort;
        },
        request);
}

bool Contains(const std::string& text, const char* fragment)
{
    return text.find(fragment) != std::string::npos;
}

TargetProbeOutcome ClassifyCommunicationFailure(
    const std::string& error,
    const std::atomic<bool>& cancellationRequested)
{
    if (cancellationRequested.load())
        return TargetProbeOutcome::Cancelled;

    if (Contains(error, "malformed")
        || Contains(error, "Malformed")
        || Contains(error, "invalid")
        || Contains(error, "Unexpected")
        || Contains(error, "truncated")
        || Contains(error, "syntax"))
    {
        return TargetProbeOutcome::MalformedResponse;
    }

    if (Contains(error, "timed out")
        || Contains(error, "No matching")
        || Contains(error, "Incomplete")
        || Contains(error, "did not respond"))
    {
        return TargetProbeOutcome::Timeout;
    }

    return TargetProbeOutcome::TransportUnavailable;
}

std::string FailureMessage(
    const TargetProbeOutcome outcome,
    const std::string& error)
{
    switch (outcome)
    {
    case TargetProbeOutcome::Cancelled:
        return "The read-only target probe was cancelled.";
    case TargetProbeOutcome::Timeout:
        return "The read-only target probe timed out: " + error;
    case TargetProbeOutcome::MalformedResponse:
        return "The controller returned a malformed response: " + error;
    default:
        return "The read-only target probe could not communicate: " + error;
    }
}

} // namespace

TargetProbeResult RunTargetProbe(
    ITransportFactory& transportFactory,
    const TargetProbeRequest& request,
    const std::atomic<bool>& cancellationRequested)
{
    TargetProbeResult result;
    result.evidence.noControlTaken = true;
    result.evidence.noStateChangingCommandsSent = true;

    if (cancellationRequested.load())
    {
        result.outcome = TargetProbeOutcome::Cancelled;
        result.message = FailureMessage(result.outcome, {});
        return result;
    }

    auto created = transportFactory.Create(request.endpoint);
    if (!created.session)
    {
        result.outcome = ClassifyCommunicationFailure(
            created.error, cancellationRequested);
        result.message = FailureMessage(result.outcome, created.error);
        return result;
    }

    auto session = std::move(created.session);
    const auto finish = [&](TargetProbeResult current) {
        session->Close();
        current.temporaryConnectionClosed = true;
        return current;
    };

    auto* transport = session->CommandTransport();
    if (!transport)
    {
        result.outcome = TargetProbeOutcome::TransportUnavailable;
        result.message = FailureMessage(
            result.outcome,
            "the transport factory returned no command transport");
        return finish(std::move(result));
    }

    const auto opened = transport->Open(
        EndpointHost(request.endpoint),
        EndpointPort(request.endpoint));
    if (!opened.success)
    {
        result.outcome = ClassifyCommunicationFailure(
            opened.error, cancellationRequested);
        result.message = FailureMessage(result.outcome, opened.error);
        return finish(std::move(result));
    }
    result.temporaryConnectionOpened = true;

    std::uint16_t nextReference = 1U;
    const auto mvlc = ReadLocalRegisters(
        *transport,
        TargetProbeMvlcRegisterOrder.data(),
        TargetProbeMvlcRegisterOrder.size(),
        nextReference,
        cancellationRequested);
    if (!mvlc.success)
    {
        result.outcome = ClassifyCommunicationFailure(
            mvlc.error, cancellationRequested);
        result.message = FailureMessage(result.outcome, mvlc.error);
        return finish(std::move(result));
    }
    if (mvlc.values.size() != TargetProbeMvlcRegisterOrder.size())
    {
        result.outcome = TargetProbeOutcome::MalformedResponse;
        result.message = FailureMessage(
            result.outcome,
            "unexpected MVLC identity response size");
        return finish(std::move(result));
    }

    result.evidence.controllerConnected = true;
    result.mvlcHardwareId = mvlc.values[0U];
    result.mvlcFirmwareRevision = mvlc.values[1U];
    result.mvlcDaqMode = mvlc.values[2U];

    if (*result.mvlcHardwareId != TargetProbeExpectedMvlcHardwareId)
    {
        result.outcome = TargetProbeOutcome::WrongMvlcIdentity;
        result.message = "The connected controller is not the supported MVLC.";
        return finish(std::move(result));
    }
    if (*result.mvlcFirmwareRevision != TargetProbeExpectedMvlcFirmware)
    {
        result.outcome = TargetProbeOutcome::WrongMvlcFirmware;
        result.message =
            "The MVLC firmware is not the supported FW0046 revision.";
        return finish(std::move(result));
    }
    result.evidence.controllerIdentityAndFirmwareVerified = true;

    if (*result.mvlcDaqMode != 0U)
    {
        result.outcome = TargetProbeOutcome::ControllerDaqActive;
        result.evidence.activeControllerUseDetected = true;
        result.message = ActiveUseMessage;
        return finish(std::move(result));
    }
    result.evidence.controllerDaqIdleVerified = true;

    std::uint16_t nextSuperReference = nextReference;
    std::uint32_t nextStackReference = 1U;
    const auto readTarget = [&](const std::uint16_t registerOffset) {
        return ReadVmeD16(
            *transport,
            request.targetAddress.FullA32Value() + registerOffset,
            nextSuperReference,
            nextStackReference,
            cancellationRequested);
    };
    const auto failRead = [&](const MvlcVmeReadResult& read) {
        result.outcome = ClassifyCommunicationFailure(
            read.error, cancellationRequested);
        result.message = FailureMessage(result.outcome, read.error);
        return finish(std::move(result));
    };

    const auto hardware = readTarget(TargetProbeMdppRegisterOrder[0U]);
    if (!hardware.success)
        return failRead(hardware);
    result.targetHardwareId = hardware.value;
    if (hardware.value != Mdpp32HardwareId)
    {
        result.outcome = TargetProbeOutcome::WrongTargetIdentity;
        result.message =
            "The selected target is not the supported MDPP-32 SCP.";
        return finish(std::move(result));
    }

    const auto firmware = readTarget(TargetProbeMdppRegisterOrder[1U]);
    if (!firmware.success)
        return failRead(firmware);
    result.targetFirmwareRevision = firmware.value;
    if (firmware.value != Mdpp32ScpFirmwareRevisionFw2051)
    {
        result.outcome = TargetProbeOutcome::WrongTargetFirmware;
        result.message =
            "The selected target is not running exact FW2051.";
        return finish(std::move(result));
    }
    result.evidence.targetIdentityAndFirmwareVerified = true;

    const auto acquisition = readTarget(TargetProbeMdppRegisterOrder[2U]);
    if (!acquisition.success)
        return failRead(acquisition);
    result.targetAcquisitionControl = acquisition.value;
    if (acquisition.value != Fw2051StopAcquisitionValue)
    {
        result.outcome = TargetProbeOutcome::TargetAcquisitionActive;
        result.evidence.activeControllerUseDetected = true;
        result.message = ActiveUseMessage;
        return finish(std::move(result));
    }

    result.evidence.targetAcquisitionStoppedVerified = true;
    result.outcome = TargetProbeOutcome::VerifiedIdle;
    result.message = "The controller and target are verified idle.";
    return finish(std::move(result));
}

} // namespace fidget
