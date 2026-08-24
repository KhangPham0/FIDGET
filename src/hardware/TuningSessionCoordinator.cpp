#include "hardware/TuningSessionCoordinator.h"

#include "hardware/CommandWorker.h"
#include "hardware/TargetProbeOperation.h"
#include "hardware/TransportFactory.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace fidget {
namespace {

constexpr std::size_t SessionPathAttemptLimit = 64U;
std::atomic<std::uint64_t> SessionPathSequence{0U};

struct SessionPathResult
{
    bool success = false;
    std::string message;
    std::string activityLogPath;
    std::string recoveryJournalPath;
};

TransportEndpointRequest MakeEndpointRequest(
    const TunerTargetInput& input)
{
    if (input.endpointKind == TunerTargetEndpointKind::SshBridge)
    {
        return SshBridgeEndpointRequest{
            input.mvlcHost,
            input.mvlcCommandPort,
            input.sshDestination,
            input.remoteBridgeCommand,
        };
    }

    return DirectEthernetEndpointRequest{
        input.mvlcHost,
        input.mvlcCommandPort,
    };
}

SessionPathResult MakeSessionPaths(
    const ApplicationStoragePaths& storagePaths)
{
    SessionPathResult result;
    const auto ready = EnsureApplicationStorageDirectories(storagePaths);
    if (!ready.success)
    {
        result.message = ready.message;
        return result;
    }

    const auto ticks = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    for (std::size_t attempt = 0U;
         attempt < SessionPathAttemptLimit;
         ++attempt)
    {
        const auto sequence = SessionPathSequence.fetch_add(1U);
        const auto stem = "tuning-session-" + std::to_string(ticks)
            + '-' + std::to_string(sequence);
        const auto activity = storagePaths.logsDirectory
            / (stem + ".activity");
        const auto recovery = storagePaths.recoveryDirectory
            / (stem + ".recovery");

        std::error_code activityError;
        const bool activityExists = std::filesystem::exists(
            activity, activityError);
        if (activityError)
        {
            result.message =
                "Could not inspect the application activity-log path: "
                + activityError.message() + '.';
            return result;
        }

        std::error_code recoveryError;
        const bool recoveryExists = std::filesystem::exists(
            recovery, recoveryError);
        if (recoveryError)
        {
            result.message =
                "Could not inspect the application recovery path: "
                + recoveryError.message() + '.';
            return result;
        }

        if (activityExists || recoveryExists)
            continue;

        result.success = true;
        result.message = "New-session storage paths are ready.";
        result.activityLogPath = activity.string();
        result.recoveryJournalPath = recovery.string();
        return result;
    }

    result.message = "Could not allocate unique new-session storage paths.";
    return result;
}

TunerStatusLevel ProbeStatusLevel(const TargetProbeOutcome outcome)
{
    switch (outcome)
    {
    case TargetProbeOutcome::VerifiedIdle:
        return TunerStatusLevel::Success;
    case TargetProbeOutcome::ControllerDaqActive:
    case TargetProbeOutcome::TargetAcquisitionActive:
    case TargetProbeOutcome::Cancelled:
        return TunerStatusLevel::Warning;
    default:
        return TunerStatusLevel::Error;
    }
}

ApplicationStorageResult RememberBridgeFields(
    const ApplicationStoragePaths& storagePaths,
    const TunerTargetInput& input)
{
    ApplicationPreferences preferences;
    const auto loaded = LoadApplicationPreferences(storagePaths);
    if (loaded.success)
    {
        preferences = loaded.preferences;
    }
    else if (!loaded.fileMissing)
    {
        return {
            false,
            "Could not load the existing connection preferences: "
                + loaded.message,
        };
    }

    preferences.sshDestination = input.sshDestination;
    preferences.remoteBridgeCommand = input.remoteBridgeCommand;
    return SaveApplicationPreferences(storagePaths, preferences);
}

} // namespace

TuningSessionCoordinator::TuningSessionCoordinator(
    ITransportFactory& transportFactory,
    CommandWorker& worker,
    ApplicationStoragePaths storagePaths,
    TuningSessionSnapshotReader snapshotReader,
    TuningSessionSnapshotPublisher snapshotPublisher)
    : transportFactory_(transportFactory)
    , worker_(worker)
    , storagePaths_(std::move(storagePaths))
    , snapshotReader_(std::move(snapshotReader))
    , snapshotPublisher_(std::move(snapshotPublisher))
{
}

bool TuningSessionCoordinator::Handles(
    const TunerCommand& command) noexcept
{
    return std::holds_alternative<EditTunerTargetCommand>(command)
        || std::holds_alternative<SelectTunerTargetCommand>(command)
        || std::holds_alternative<ProbeTunerTargetCommand>(command)
        || std::holds_alternative<OpenTunerTargetSessionCommand>(command)
        || std::holds_alternative<ClearTunerTargetCommand>(command);
}

void TuningSessionCoordinator::Submit(TunerCommand command)
{
    if (std::holds_alternative<EditTunerTargetCommand>(command))
    {
        CancelPendingProbe();
        auto request = std::get<EditTunerTargetCommand>(std::move(command));
        (void)worker_.Post(
            [this, request = std::move(request)]() mutable {
                EditTarget(std::move(request));
            });
        return;
    }
    if (std::holds_alternative<SelectTunerTargetCommand>(command))
    {
        CancelPendingProbe();
        (void)worker_.Post([this] { SelectTarget(); });
        return;
    }
    if (std::holds_alternative<ProbeTunerTargetCommand>(command))
    {
        auto cancellation = BeginProbe();
        (void)worker_.Post([this, cancellation = std::move(cancellation)] {
            ProbeTarget(cancellation);
        });
        return;
    }
    if (std::holds_alternative<OpenTunerTargetSessionCommand>(command))
    {
        (void)worker_.Post([this] { OpenTargetSession(); });
        return;
    }
    if (std::holds_alternative<ClearTunerTargetCommand>(command))
    {
        CancelPendingProbe();
        (void)worker_.Post([this] { ClearTarget(); });
    }
}

void TuningSessionCoordinator::CancelPendingProbe() noexcept
{
    const std::lock_guard<std::mutex> lock(probeMutex_);
    if (activeProbeCancellation_)
        activeProbeCancellation_->store(true);
}

void TuningSessionCoordinator::EditTarget(EditTunerTargetCommand command)
{
    auto snapshot = SnapshotCopy();
    snapshot.target.input = std::move(command.input);
    snapshot.target.verification.inProgress = false;
    snapshot.target.verification.invalidated = true;
    snapshot.target.sessionGate = {};
    RefreshPresentationEvidence(snapshot);
    PublishStatus(
        std::move(snapshot),
        TunerStatusLevel::Information,
        "The tuner target fields were updated. Verification is no longer "
        "current.");
}

void TuningSessionCoordinator::SelectTarget()
{
    auto snapshot = SnapshotCopy();
    const auto validation = ValidateTunerTargetInput(snapshot.target.input);
    snapshot.target.verification.inProgress = false;
    snapshot.target.verification.invalidated = true;
    snapshot.target.sessionGate = {};

    if (!validation.success || !validation.normalizedModuleAddress)
    {
        snapshot.target.selection.reset();
        RefreshPresentationEvidence(snapshot);
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Warning,
            "The tuner target was not selected.",
            validation.endpointValid
                ? validation.moduleAddressMessage
                : validation.endpointMessage);
        return;
    }

    snapshot.target.selection = TunerTargetSelection{
        snapshot.target.input,
        *validation.normalizedModuleAddress,
    };
    const auto remembered = RememberBridgeFields(
        storagePaths_, snapshot.target.input);
    RefreshPresentationEvidence(snapshot);
    PublishStatus(
        std::move(snapshot),
        remembered.success
            ? TunerStatusLevel::Success
            : TunerStatusLevel::Warning,
        "The endpoint request and normalized target were selected.",
        remembered.success
            ? "Run Check to connect and verify the controller and target."
            : "The SSH connection fields could not be remembered: "
                + remembered.message);
}

void TuningSessionCoordinator::ProbeTarget(
    const std::shared_ptr<std::atomic<bool>>& cancellation)
{
    auto snapshot = SnapshotCopy();
    if (!snapshot.target.selection
        || snapshot.target.selection->input != snapshot.target.input)
    {
        snapshot.target.verification.inProgress = false;
        snapshot.target.verification.invalidated = true;
        snapshot.target.verification.result = {};
        snapshot.target.verification.result.message =
            "Select the current target fields before running the read-only "
            "probe.";
        RefreshPresentationEvidence(snapshot);
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Warning,
            "The read-only target probe was not started.",
            "Select the current target fields first.");
        FinishProbe(cancellation);
        return;
    }

    const auto selected = *snapshot.target.selection;
    snapshot.target.verification.inProgress = true;
    snapshot.target.verification.invalidated = false;
    snapshot.target.verification.probedInput = selected.input;
    snapshot.target.verification.result = {};
    snapshot.target.verification.result.outcome =
        TargetProbeOutcome::InProgress;
    snapshot.target.verification.result.message =
        "Running the read-only target probe.";
    snapshot.target.sessionGate = {};
    RefreshPresentationEvidence(snapshot);
    PublishStatus(
        std::move(snapshot),
        TunerStatusLevel::Information,
        "Running the read-only target probe.");

    const auto result = RunTargetProbe(
        transportFactory_,
        TargetProbeRequest{
            MakeEndpointRequest(selected.input),
            selected.moduleAddress,
        },
        *cancellation);

    std::string persistenceError;
    if (result.outcome == TargetProbeOutcome::VerifiedIdle)
    {
        const ApplicationPreferences preferences{
            selected.input.mvlcHost,
            selected.input.moduleAddress,
            selected.input.sshDestination,
            selected.input.remoteBridgeCommand,
        };
        const auto saved = SaveApplicationPreferences(
            storagePaths_, preferences);
        if (!saved.success)
            persistenceError = saved.message;
    }

    snapshot = SnapshotCopy();
    snapshot.target.verification.inProgress = false;
    snapshot.target.verification.invalidated = false;
    snapshot.target.verification.probedInput = selected.input;
    snapshot.target.verification.result = result;
    RefreshPresentationEvidence(snapshot);
    PublishStatus(
        std::move(snapshot),
        persistenceError.empty()
            ? ProbeStatusLevel(result.outcome)
            : TunerStatusLevel::Warning,
        result.message,
        persistenceError.empty()
            ? std::string{}
            : "Verification succeeded, but the remembered target could not "
              "be saved: " + persistenceError);
    FinishProbe(cancellation);
}

void TuningSessionCoordinator::OpenTargetSession()
{
    auto snapshot = SnapshotCopy();
    snapshot.target.sessionGate = {};
    if (!TargetVerificationIsFresh(snapshot.target))
    {
        snapshot.target.sessionGate.outcome =
            TunerTargetSessionGateOutcome::RefusedVerificationNotFresh;
        snapshot.target.sessionGate.message =
            "The target session was not opened because fresh target "
            "verification is required.";
        RefreshPresentationEvidence(snapshot);
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Warning,
            "The target session was not opened.",
            "Run a successful read-only target probe for the current fields "
            "first.");
        return;
    }

    const auto paths = MakeSessionPaths(storagePaths_);
    if (!paths.success)
    {
        snapshot.target.sessionGate.outcome =
            TunerTargetSessionGateOutcome::RefusedStorageUnavailable;
        snapshot.target.sessionGate.message =
            "The target session was not opened because application storage "
            "is unavailable.";
        RefreshPresentationEvidence(snapshot);
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Error,
            "The target session was not opened.",
            paths.message);
        return;
    }

    snapshot.target.sessionGate.outcome =
        TunerTargetSessionGateOutcome::ReadyForPreparation;
    snapshot.target.sessionGate.message =
        "Fresh target verification passed. Session preparation has not "
        "started.";
    snapshot.target.sessionGate.activityLogPath = paths.activityLogPath;
    snapshot.target.sessionGate.recoveryJournalPath =
        paths.recoveryJournalPath;
    RefreshPresentationEvidence(snapshot);
    PublishStatus(
        std::move(snapshot),
        TunerStatusLevel::Success,
        "The target is ready for later session preparation.",
        "No session hardware work was performed.");
}

void TuningSessionCoordinator::ClearTarget()
{
    auto snapshot = SnapshotCopy();
    const bool cancelledProbe =
        snapshot.target.verification.result.outcome
        == TargetProbeOutcome::Cancelled;
    snapshot.target = {};
    RefreshPresentationEvidence(snapshot);
    PublishStatus(
        std::move(snapshot),
        TunerStatusLevel::Information,
        cancelledProbe
            ? "The read-only target probe was cancelled and the tuner target "
              "was cleared."
            : "The tuner target was cleared.");
}

std::shared_ptr<std::atomic<bool>>
TuningSessionCoordinator::BeginProbe()
{
    const std::lock_guard<std::mutex> lock(probeMutex_);
    if (activeProbeCancellation_)
        activeProbeCancellation_->store(true);
    activeProbeCancellation_ =
        std::make_shared<std::atomic<bool>>(false);
    return activeProbeCancellation_;
}

void TuningSessionCoordinator::FinishProbe(
    const std::shared_ptr<std::atomic<bool>>& cancellation) noexcept
{
    const std::lock_guard<std::mutex> lock(probeMutex_);
    if (activeProbeCancellation_ == cancellation)
        activeProbeCancellation_.reset();
}

TunerSnapshot TuningSessionCoordinator::SnapshotCopy() const
{
    const auto current = snapshotReader_ ? snapshotReader_() : nullptr;
    return current ? *current : TunerSnapshot{};
}

void TuningSessionCoordinator::PublishStatus(
    TunerSnapshot snapshot,
    const TunerStatusLevel level,
    std::string summary,
    std::string detail)
{
    snapshot.statusMessages.clear();
    snapshot.statusMessages.push_back({
        level,
        std::move(summary),
        std::move(detail),
    });
    if (snapshotPublisher_)
        snapshotPublisher_(std::move(snapshot));
}

void TuningSessionCoordinator::RefreshPresentationEvidence(
    TunerSnapshot& snapshot)
{
    ApplyTargetPresentationEvidence(
        snapshot.target,
        snapshot.tuningSession.evidence);
    const bool currentSelection = snapshot.target.selection.has_value()
        && snapshot.target.selection->input == snapshot.target.input;
    const auto validation = ValidateTunerTargetInput(snapshot.target.input);
    snapshot.tuningSession.evidence.endpointInputsValid =
        validation.success;
    snapshot.tuningSession.evidence.endpointEditingAllowed =
        !snapshot.target.verification.inProgress;
    snapshot.tuningSession.evidence.operationIdle =
        !snapshot.target.verification.inProgress
        && snapshot.activeOperation == GuidedTunerOperation::None;
    if (snapshot.tuningSession.phase == TuningSessionPhase::Home
        && !snapshot.tuningSession.evidence.recoveryContextEstablished
        && !snapshot.tuningSession.evidence.recoveryInProgress)
    {
        snapshot.tuningSession.evidence.noRecoveryPending =
            !snapshot.recoveryRecordAvailable
            && snapshot.recoveryJournalStatus == RecoveryJournalStatus::None;
        snapshot.tuningSession.evidence.helpAvailable = true;
        snapshot.tuningSession.evidence.detailsAvailable = currentSelection;
        snapshot.tuningSession.evidence.logsAvailable =
            !snapshot.statusMessages.empty();
        snapshot.tuningSession.evidence.primaryNavigationAvailable = true;
        snapshot.tuningSession.evidence.navigationAwayVerifiedSafe =
            snapshot.tuningSession.evidence.noRecoveryPending
            && !snapshot.tuningSession.evidence.controlHeld
            && (!snapshot.tuningSession.evidence.activeControllerUseDetected
                || (snapshot.tuningSession.evidence.noControlTaken
                    && snapshot.tuningSession.evidence
                        .noStateChangingCommandsSent));
    }
}

} // namespace fidget
