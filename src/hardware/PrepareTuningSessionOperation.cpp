#include "hardware/PrepareTuningSessionOperation.h"

#include "hardware/TargetProbeOperation.h"
#include "hardware/VmeTransaction.h"

#include <array>
#include <filesystem>
#include <string>
#include <utility>
#include <variant>

namespace fidget {
namespace {

constexpr std::uint16_t DaqModeRegister = 0x1300U;
constexpr std::uint16_t InitialDaqReference = 0x5000U;

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

ControllerEndpointRequest RecoveryEndpoint(
    const TransportEndpointRequest& request)
{
    if (const auto* direct =
            std::get_if<DirectEthernetEndpointRequest>(&request))
    {
        return {
            ControllerEndpointKind::DirectEthernet,
            direct->mvlcHost,
            direct->mvlcCommandPort,
            {},
            {},
        };
    }
    const auto& bridge = std::get<SshBridgeEndpointRequest>(request);
    return {
        ControllerEndpointKind::SshBridge,
        bridge.mvlcHost,
        bridge.mvlcCommandPort,
        bridge.sshDestination,
        bridge.remoteBridgeCommand,
    };
}

std::filesystem::path ComparablePath(const std::filesystem::path& path)
{
    std::error_code error;
    auto absolute = std::filesystem::absolute(path, error);
    return error ? path.lexically_normal() : absolute.lexically_normal();
}

bool RecoveryPathBelongsToStorage(
    const PrepareTuningSessionRequest& request)
{
    if (request.recoveryJournalPath.empty()
        || request.recoveryJournalPath.filename().empty()
        || request.storagePaths.recoveryDirectory.empty())
    {
        return false;
    }
    return ComparablePath(request.recoveryJournalPath.parent_path())
        == ComparablePath(request.storagePaths.recoveryDirectory);
}

PrepareTuningSessionOutcome OutcomeForPreflight(
    const TargetProbeOutcome outcome)
{
    switch (outcome)
    {
    case TargetProbeOutcome::TransportUnavailable:
    case TargetProbeOutcome::Timeout:
    case TargetProbeOutcome::MalformedResponse:
        return PrepareTuningSessionOutcome::TransportUnavailable;
    case TargetProbeOutcome::Cancelled:
        return PrepareTuningSessionOutcome::Interrupted;
    default:
        return PrepareTuningSessionOutcome::PreflightRefused;
    }
}

TunerRecoveryRecord PreparedRecord(
    const PrepareTuningSessionRequest& request,
    const TargetProbeResult& probe)
{
    TunerRecoveryV5Data data;
    data.sessionPhase = TuningSessionPhase::Preparing;
    data.endpoint = RecoveryEndpoint(request.endpoint);
    data.identity = {
        probe.mvlcHardwareId.value(),
        probe.mvlcFirmwareRevision.value(),
        request.targetAddress.FullA32Value(),
        probe.targetHardwareId.value(),
        probe.targetFirmwareRevision.value(),
    };
    data.selectorParkingRequired = true;

    TunerRecoveryRecord record;
    record.formatVersion = TunerRecoveryJournalV5FormatVersion;
    record.version5 = std::move(data);
    return record;
}

} // namespace

PrepareTuningSessionResult PrepareTuningSession(
    ITransportFactory& transportFactory,
    const PrepareTuningSessionRequest& request,
    const std::atomic<bool>& cancellationRequested,
    const PrepareTuningSessionRuntime& runtime)
{
    PrepareTuningSessionResult result;
    result.recoveryJournalPath = request.recoveryJournalPath.string();

    if (!RecoveryPathBelongsToStorage(request))
    {
        result.outcome = PrepareTuningSessionOutcome::StorageUnavailable;
        result.message = "The recovery journal is not inside the application "
                         "storage recovery directory.";
        return result;
    }
    if (cancellationRequested.load())
    {
        result.outcome = PrepareTuningSessionOutcome::Interrupted;
        result.message = "Session preparation was cancelled before it began.";
        return result;
    }

    auto created = transportFactory.Create(request.endpoint);
    if (!created.session)
    {
        result.outcome = PrepareTuningSessionOutcome::TransportUnavailable;
        result.message = "Could not create the session-preparation transport: "
            + created.error;
        return result;
    }
    auto session = std::move(created.session);
    const auto finish = [&](PrepareTuningSessionResult current) {
        session->Close();
        current.temporaryConnectionClosed = true;
        return current;
    };

    auto* transport = session->CommandTransport();
    if (transport == nullptr)
    {
        result.outcome = PrepareTuningSessionOutcome::TransportUnavailable;
        result.message =
            "The session-preparation factory returned no command transport.";
        return finish(std::move(result));
    }
    const auto opened = transport->Open(
        EndpointHost(request.endpoint), EndpointPort(request.endpoint));
    if (!opened.success)
    {
        result.outcome = PrepareTuningSessionOutcome::TransportUnavailable;
        result.message = "Could not open the session-preparation transport: "
            + opened.error;
        return finish(std::move(result));
    }
    result.temporaryConnectionOpened = true;

    result.preflight = ProbeTargetOnOpenTransport(
        *transport,
        TargetProbeRequest{request.endpoint, request.targetAddress},
        cancellationRequested);
    if (result.preflight.outcome != TargetProbeOutcome::VerifiedIdle)
    {
        result.outcome = OutcomeForPreflight(result.preflight.outcome);
        result.message = result.preflight.message;
        return finish(std::move(result));
    }

    // Read-only Check recognizes both supported templates, but this is the
    // operation's independent write authorization gate. Widen the named
    // write-approved set only after the corresponding hardware acceptance.
    if (!result.preflight.targetHardwareId.has_value()
        || !IsWriteApprovedMdpp32HardwareId(
            *result.preflight.targetHardwareId))
    {
        result.outcome = PrepareTuningSessionOutcome::PreflightRefused;
        result.message =
            "Session preparation refused the MDPP-32 v2 target. Write "
            "support for hardware ID 0x500C awaits recorded hardware "
            "acceptance.";
        return finish(std::move(result));
    }

    const auto storageReady = EnsureApplicationStorageDirectories(
        request.storagePaths);
    if (!storageReady.success)
    {
        result.outcome = PrepareTuningSessionOutcome::StorageUnavailable;
        result.message = "Session preparation wrote no hardware state: "
            + storageReady.message;
        return finish(std::move(result));
    }

    auto record = PreparedRecord(request, result.preflight);
    const auto createdJournal = CreateTunerRecoveryJournalExclusive(
        record,
        result.recoveryJournalPath,
        runtime.journalRuntime);
    if (!createdJournal.success)
    {
        result.journalCreatedExclusively =
            createdJournal.destinationInstalled;
        result.journalPhase = createdJournal.destinationInstalled
            ? PrepareTuningSessionJournalPhase::Prepared
            : PrepareTuningSessionJournalPhase::None;
        result.outcome = createdJournal.destinationAlreadyExists
            ? PrepareTuningSessionOutcome::RecoveryRecordExists
            : PrepareTuningSessionOutcome::StorageUnavailable;
        result.message = "Session preparation wrote no selector state: "
            + createdJournal.message;
        return finish(std::move(result));
    }
    result.journalCreatedExclusively = true;
    result.journalPhase = PrepareTuningSessionJournalPhase::Prepared;

    const auto advance = [&](const PrepareTuningSessionCheckpointKind kind,
                             const std::optional<std::uint16_t> quad,
                             const std::uint16_t registerOffset) {
        PrepareTuningSessionCheckpoint event{
            kind,
            quad,
            registerOffset,
            result.completedCheckpoints,
        };
        ++result.completedCheckpoints;
        if (runtime.checkpoint && !runtime.checkpoint(event))
        {
            result.interruptedAt = event;
            return false;
        }
        return true;
    };

    if (!advance(
            PrepareTuningSessionCheckpointKind::JournalPrepared,
            std::nullopt,
            0U))
    {
        result.outcome = PrepareTuningSessionOutcome::Interrupted;
        result.message = "Session preparation stopped after the durable "
                         "Prepared journal was installed.";
        return finish(std::move(result));
    }

    std::uint16_t nextDaqReference = InitialDaqReference;
    const std::array<std::uint16_t, 1U> daqAddress{{DaqModeRegister}};
    const auto daqGate = [&](const std::string& boundary) {
        if (cancellationRequested.load())
        {
            return ScpCaptureGateResult{
                ScpCaptureGateStatus::Cancelled,
                "Session preparation was cancelled at " + boundary + '.',
            };
        }
        const auto daq = ReadLocalRegisters(
            *transport,
            daqAddress.data(),
            daqAddress.size(),
            nextDaqReference,
            cancellationRequested);
        if (!daq.success || daq.values.size() != 1U)
        {
            return ScpCaptureGateResult{
                ScpCaptureGateStatus::CommunicationUnavailable,
                daq.success
                    ? "The DAQ-idle gate returned an invalid value count at "
                        + boundary + '.'
                    : "Could not reverify DAQ idleness at " + boundary
                        + ": " + daq.error,
            };
        }
        ++result.daqIdleChecks;
        if (daq.values.front() != 0U)
        {
            result.controllerBecameActive = true;
            return ScpCaptureGateResult{
                ScpCaptureGateStatus::OwnershipLost,
                "Controller acquisition became active at " + boundary
                    + ". No further hardware write was sent; the Prepared "
                      "journal was retained.",
            };
        }
        if (!advance(
                PrepareTuningSessionCheckpointKind::DaqIdleVerified,
                std::nullopt,
                DaqModeRegister))
        {
            return ScpCaptureGateResult{
                ScpCaptureGateStatus::Cancelled,
                "Session preparation was interrupted after a DAQ-idle "
                "verification.",
            };
        }
        return ScpCaptureGateResult{
            ScpCaptureGateStatus::Allowed,
            "DAQ mode remains idle.",
        };
    };

    ScpCaptureRuntime captureRuntime;
    captureRuntime.delay = runtime.delay;
    captureRuntime.gateBeforeFirstSelector = true;
    captureRuntime.checkpoint = [&](const ScpCaptureCheckpoint& event) {
        PrepareTuningSessionCheckpointKind kind;
        switch (event.kind)
        {
        case ScpCaptureCheckpointKind::GlobalRegisterRead:
            kind = PrepareTuningSessionCheckpointKind::GlobalRegisterRead;
            break;
        case ScpCaptureCheckpointKind::SelectorSettled:
            kind = PrepareTuningSessionCheckpointKind::SelectorSettled;
            break;
        case ScpCaptureCheckpointKind::BankedRegisterRead:
            kind = PrepareTuningSessionCheckpointKind::BankedRegisterRead;
            break;
        case ScpCaptureCheckpointKind::SelectorParked:
            kind = PrepareTuningSessionCheckpointKind::SelectorParked;
            break;
        }
        return advance(kind, event.quad, event.registerOffset);
    };

    const auto captured = CaptureFw2051ScpConfiguration(
        *transport,
        request.targetAddress.FullA32Value(),
        cancellationRequested,
        daqGate,
        captureRuntime);
    result.liveRestoreSnapshot = captured.configuration;
    if (captured.configuration.state != ScpConfigurationState::Complete)
    {
        result.outcome = result.controllerBecameActive
            ? PrepareTuningSessionOutcome::ControllerBecameActive
            : result.interruptedAt.has_value()
                || cancellationRequested.load()
                ? PrepareTuningSessionOutcome::Interrupted
                : PrepareTuningSessionOutcome::CaptureFailed;
        result.message = captured.configuration.message;
        return finish(std::move(result));
    }

    if (!advance(
            PrepareTuningSessionCheckpointKind::CaptureCompleted,
            std::nullopt,
            0U))
    {
        result.outcome = PrepareTuningSessionOutcome::Interrupted;
        result.message = "Session preparation stopped after capture and "
                         "selector parking. The Prepared journal remains.";
        return finish(std::move(result));
    }
    const auto finalIdle = daqGate("the snapshot-promotion boundary");
    if (finalIdle.status != ScpCaptureGateStatus::Allowed)
    {
        result.outcome = result.controllerBecameActive
            ? PrepareTuningSessionOutcome::ControllerBecameActive
            : PrepareTuningSessionOutcome::Interrupted;
        result.message = finalIdle.message;
        return finish(std::move(result));
    }
    if (!advance(
            PrepareTuningSessionCheckpointKind::BeforeJournalPromotion,
            std::nullopt,
            0U))
    {
        result.outcome = PrepareTuningSessionOutcome::Interrupted;
        result.message = "Session preparation stopped before journal "
                         "promotion. The Prepared journal remains.";
        return finish(std::move(result));
    }

    record.version5->selectorParkingRequired = false;
    record.version5->liveRestoreSnapshot = captured.configuration;
    const auto promoted = PromoteTunerRecoveryJournalV5Snapshot(
        record,
        result.recoveryJournalPath,
        runtime.journalRuntime);
    if (!promoted.success)
    {
        if (promoted.destinationInstalled)
        {
            result.journalPromotedAtomically = true;
            result.journalPhase =
                PrepareTuningSessionJournalPhase::SnapshotCaptured;
        }
        result.outcome = PrepareTuningSessionOutcome::JournalPromotionFailed;
        result.message = "The complete live snapshot could not replace its "
                         "Prepared journal: " + promoted.message;
        return finish(std::move(result));
    }
    result.journalPromotedAtomically = true;
    result.journalPhase = PrepareTuningSessionJournalPhase::SnapshotCaptured;
    if (!advance(
            PrepareTuningSessionCheckpointKind::JournalPromoted,
            std::nullopt,
            0U))
    {
        result.outcome = PrepareTuningSessionOutcome::Interrupted;
        result.message = "Session preparation stopped after the complete "
                         "snapshot journal was installed.";
        return finish(std::move(result));
    }

    result.outcome = PrepareTuningSessionOutcome::SnapshotCaptured;
    result.message = "Captured the complete 141-value live FW2051 restore "
                     "snapshot under the v5 recovery journal.";
    return finish(std::move(result));
}

} // namespace fidget
