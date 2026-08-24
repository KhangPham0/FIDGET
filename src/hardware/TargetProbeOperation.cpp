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

enum class CommunicationFailure
{
    TransportUnavailable,
    Timeout,
    MalformedResponse,
    Cancelled,
};

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

CommunicationFailure ClassifyCommunicationFailure(
    const std::string& error,
    const std::atomic<bool>& cancellationRequested)
{
    if (cancellationRequested.load())
        return CommunicationFailure::Cancelled;

    if (Contains(error, "malformed")
        || Contains(error, "Malformed")
        || Contains(error, "invalid")
        || Contains(error, "Unexpected")
        || Contains(error, "truncated")
        || Contains(error, "syntax"))
    {
        return CommunicationFailure::MalformedResponse;
    }

    if (Contains(error, "timed out")
        || Contains(error, "No matching")
        || Contains(error, "Incomplete")
        || Contains(error, "did not respond"))
    {
        return CommunicationFailure::Timeout;
    }

    return CommunicationFailure::TransportUnavailable;
}

ControllerProbeOutcome ControllerOutcome(
    const CommunicationFailure failure)
{
    switch (failure)
    {
    case CommunicationFailure::Cancelled:
        return ControllerProbeOutcome::Cancelled;
    case CommunicationFailure::Timeout:
        return ControllerProbeOutcome::Timeout;
    case CommunicationFailure::MalformedResponse:
        return ControllerProbeOutcome::MalformedResponse;
    case CommunicationFailure::TransportUnavailable:
        return ControllerProbeOutcome::TransportUnavailable;
    }
    return ControllerProbeOutcome::TransportUnavailable;
}

TargetProbeOutcome TargetOutcome(const CommunicationFailure failure)
{
    switch (failure)
    {
    case CommunicationFailure::Cancelled:
        return TargetProbeOutcome::Cancelled;
    case CommunicationFailure::Timeout:
        return TargetProbeOutcome::Timeout;
    case CommunicationFailure::MalformedResponse:
        return TargetProbeOutcome::MalformedResponse;
    case CommunicationFailure::TransportUnavailable:
        return TargetProbeOutcome::TransportUnavailable;
    }
    return TargetProbeOutcome::TransportUnavailable;
}

std::string ControllerFailureMessage(
    const ControllerProbeOutcome outcome,
    const std::string& error)
{
    switch (outcome)
    {
    case ControllerProbeOutcome::Cancelled:
        return "The read-only controller connection was cancelled.";
    case ControllerProbeOutcome::Timeout:
        return "The read-only controller connection timed out: " + error;
    case ControllerProbeOutcome::MalformedResponse:
        return "The controller returned a malformed response: " + error;
    default:
        return "The read-only controller connection could not communicate: "
            + error;
    }
}

std::string TargetFailureMessage(
    const TargetProbeOutcome outcome,
    const std::string& error)
{
    switch (outcome)
    {
    case TargetProbeOutcome::Cancelled:
        return "The read-only target check was cancelled.";
    case TargetProbeOutcome::Timeout:
        return "The read-only target check timed out: " + error;
    case TargetProbeOutcome::MalformedResponse:
        return "The target returned a malformed response: " + error;
    default:
        return "The read-only target check could not communicate: " + error;
    }
}

} // namespace

ControllerProbeResult RunControllerProbe(
    ITransportFactory& transportFactory,
    const ControllerProbeRequest& request,
    const std::atomic<bool>& cancellationRequested)
{
    ControllerProbeResult result;
    result.evidence.noControlTaken = true;
    result.evidence.noVmeOrModuleSettingWritesSent = true;

    if (cancellationRequested.load())
    {
        result.outcome = ControllerProbeOutcome::Cancelled;
        result.message = ControllerFailureMessage(result.outcome, {});
        return result;
    }

    auto created = transportFactory.Create(request.endpoint);
    if (!created.session)
    {
        result.outcome = ControllerOutcome(ClassifyCommunicationFailure(
            created.error, cancellationRequested));
        result.message = ControllerFailureMessage(
            result.outcome, created.error);
        return result;
    }

    auto session = std::move(created.session);
    const auto finish = [&](ControllerProbeResult current) {
        session->Close();
        current.temporaryConnectionClosed = true;
        return current;
    };

    auto* transport = session->CommandTransport();
    if (!transport)
    {
        result.outcome = ControllerProbeOutcome::TransportUnavailable;
        result.message = ControllerFailureMessage(
            result.outcome,
            "the transport factory returned no command transport");
        return finish(std::move(result));
    }

    const auto opened = transport->Open(
        EndpointHost(request.endpoint), EndpointPort(request.endpoint));
    if (!opened.success)
    {
        result.outcome = ControllerOutcome(ClassifyCommunicationFailure(
            opened.error, cancellationRequested));
        result.message = ControllerFailureMessage(
            result.outcome, opened.error);
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
        result.outcome = ControllerOutcome(ClassifyCommunicationFailure(
            mvlc.error, cancellationRequested));
        result.message = ControllerFailureMessage(
            result.outcome, mvlc.error);
        return finish(std::move(result));
    }
    if (cancellationRequested.load())
    {
        result.outcome = ControllerProbeOutcome::Cancelled;
        result.message = ControllerFailureMessage(result.outcome, {});
        return finish(std::move(result));
    }
    if (mvlc.values.size() != TargetProbeMvlcRegisterOrder.size())
    {
        result.outcome = ControllerProbeOutcome::MalformedResponse;
        result.message = ControllerFailureMessage(
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
        result.outcome = ControllerProbeOutcome::WrongMvlcIdentity;
        result.message = "The connected controller is not the supported MVLC.";
        return finish(std::move(result));
    }
    if (*result.mvlcFirmwareRevision != TargetProbeExpectedMvlcFirmware)
    {
        result.outcome = ControllerProbeOutcome::WrongMvlcFirmware;
        result.message =
            "The MVLC firmware is not the supported FW0046 revision.";
        return finish(std::move(result));
    }
    result.evidence.controllerIdentityAndFirmwareVerified = true;

    if (*result.mvlcDaqMode != 0U)
    {
        result.outcome = ControllerProbeOutcome::ControllerDaqActive;
        result.evidence.activeControllerUseDetected = true;
        result.message = ActiveUseMessage;
        return finish(std::move(result));
    }

    result.evidence.controllerDaqIdleVerified = true;
    result.outcome = ControllerProbeOutcome::VerifiedIdle;
    result.message = "The MVLC controller is verified and idle.";
    return finish(std::move(result));
}

TargetProbeResult RunTargetProbe(
    ITransportFactory& transportFactory,
    const TargetProbeRequest& request,
    const std::atomic<bool>& cancellationRequested)
{
    TargetProbeResult result;
    result.evidence.noControlTaken = true;
    result.evidence.noVmeOrModuleSettingWritesSent = true;

    if (cancellationRequested.load())
    {
        result.outcome = TargetProbeOutcome::Cancelled;
        result.message = TargetFailureMessage(result.outcome, {});
        return result;
    }

    auto created = transportFactory.Create(request.endpoint);
    if (!created.session)
    {
        result.outcome = TargetOutcome(ClassifyCommunicationFailure(
            created.error, cancellationRequested));
        result.message = TargetFailureMessage(result.outcome, created.error);
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
        result.message = TargetFailureMessage(
            result.outcome,
            "the transport factory returned no command transport");
        return finish(std::move(result));
    }

    const auto opened = transport->Open(
        EndpointHost(request.endpoint), EndpointPort(request.endpoint));
    if (!opened.success)
    {
        result.outcome = TargetOutcome(ClassifyCommunicationFailure(
            opened.error, cancellationRequested));
        result.message = TargetFailureMessage(result.outcome, opened.error);
        return finish(std::move(result));
    }
    result.temporaryConnectionOpened = true;

    std::uint16_t nextSuperReference = 1U;
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
        result.outcome = TargetOutcome(ClassifyCommunicationFailure(
            read.error, cancellationRequested));
        result.message = TargetFailureMessage(result.outcome, read.error);
        return finish(std::move(result));
    };

    const auto hardware = readTarget(TargetProbeMdppRegisterOrder[0U]);
    if (!hardware.success)
        return failRead(hardware);
    if (cancellationRequested.load())
    {
        result.outcome = TargetProbeOutcome::Cancelled;
        result.message = TargetFailureMessage(result.outcome, {});
        return finish(std::move(result));
    }
    result.targetHardwareId = hardware.value;
    if (!IsSupportedMdpp32HardwareId(hardware.value))
    {
        result.outcome = TargetProbeOutcome::WrongTargetIdentity;
        result.message =
            "The selected target is not the supported MDPP-32 SCP.";
        return finish(std::move(result));
    }

    const auto firmware = readTarget(TargetProbeMdppRegisterOrder[1U]);
    if (!firmware.success)
        return failRead(firmware);
    if (cancellationRequested.load())
    {
        result.outcome = TargetProbeOutcome::Cancelled;
        result.message = TargetFailureMessage(result.outcome, {});
        return finish(std::move(result));
    }
    result.targetFirmwareRevision = firmware.value;
    if (firmware.value != Mdpp32ScpFirmwareRevisionFw2051)
    {
        result.outcome = TargetProbeOutcome::WrongTargetFirmware;
        result.message = "The selected target is not running exact FW2051.";
        return finish(std::move(result));
    }
    result.evidence.targetIdentityAndFirmwareVerified = true;

    const auto acquisition = readTarget(TargetProbeMdppRegisterOrder[2U]);
    if (!acquisition.success)
        return failRead(acquisition);
    if (cancellationRequested.load())
    {
        result.outcome = TargetProbeOutcome::Cancelled;
        result.message = TargetFailureMessage(result.outcome, {});
        return finish(std::move(result));
    }
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
    result.message = "The target module is verified and stopped.";
    return finish(std::move(result));
}

} // namespace fidget
