#include "hardware/TuningSessionCoordinator.h"

#include "hardware/CommandWorker.h"
#include "hardware/TargetProbeOperation.h"
#include "hardware/TransportFactory.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
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

TunerStatusLevel ProbeStatusLevel(const ControllerProbeOutcome outcome)
{
    switch (outcome)
    {
    case ControllerProbeOutcome::VerifiedIdle:
        return TunerStatusLevel::Success;
    case ControllerProbeOutcome::ControllerDaqActive:
    case ControllerProbeOutcome::Cancelled:
        return TunerStatusLevel::Warning;
    default:
        return TunerStatusLevel::Error;
    }
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

TunerStatusLevel WithPersistenceWarning(
    const TunerStatusLevel operationLevel,
    const bool persistenceSucceeded)
{
    if (!persistenceSucceeded
        && operationLevel == TunerStatusLevel::Success)
    {
        return TunerStatusLevel::Warning;
    }
    return operationLevel;
}

ApplicationStorageResult SaveBridgeConvenienceFields(
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
        || std::holds_alternative<ClearTunerTargetCommand>(command)
        || std::holds_alternative<SetTunerWorkspaceCommand>(command)
        || std::holds_alternative<ClearTunerWorkspaceCommand>(command);
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
        auto cancellation = BeginProbe();
        (void)worker_.Post([this, cancellation = std::move(cancellation)] {
            ConnectController(cancellation);
        });
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
        return;
    }
    if (std::holds_alternative<SetTunerWorkspaceCommand>(command))
    {
        auto request = std::get<SetTunerWorkspaceCommand>(
            std::move(command));
        (void)worker_.Post(
            [this, request = std::move(request)]() mutable {
                SetWorkspace(std::move(request));
            });
        return;
    }
    if (std::holds_alternative<ClearTunerWorkspaceCommand>(command))
    {
        (void)worker_.Post([this] { ClearWorkspace(); });
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
    const auto previousInput = snapshot.target.input;
    const bool endpointChanged =
        ControllerEndpointForTarget(previousInput)
        != ControllerEndpointForTarget(command.input);
    const bool moduleChanged =
        previousInput.moduleAddress != command.input.moduleAddress;
    snapshot.target.input = std::move(command.input);
    if (endpointChanged
        || (!moduleChanged && snapshot.target.input == previousInput))
    {
        snapshot.target.controllerVerification.inProgress = false;
        snapshot.target.controllerVerification.invalidated = true;
        snapshot.target.verification.inProgress = false;
        snapshot.target.verification.invalidated = true;
    }
    else if (moduleChanged)
    {
        snapshot.target.verification.inProgress = false;
        snapshot.target.verification.invalidated = true;
    }
    snapshot.target.sessionGate = {};
    if (moduleChanged)
        RefreshWorkspaceTargetEvidence(snapshot);
    RefreshPresentationEvidence(snapshot);
    const bool controllerStillFresh =
        ControllerVerificationIsFresh(snapshot.target);
    PublishStatus(
        std::move(snapshot),
        TunerStatusLevel::Information,
        endpointChanged
            ? "The controller and target fields were updated. Both "
              "verification stages are no longer current."
            : moduleChanged
                ? "The module address was updated. Target verification is "
                  "no longer current."
                : "The current controller and target verification was "
                  "cleared.",
        moduleChanged && controllerStillFresh
            ? "The verified controller connection remains current."
            : std::string{});
}

void TuningSessionCoordinator::ConnectController(
    const std::shared_ptr<std::atomic<bool>>& cancellation)
{
    auto snapshot = SnapshotCopy();
    const auto validation = ValidateTunerTargetInput(snapshot.target.input);
    snapshot.target.sessionGate = {};

    if (!validation.endpointValid)
    {
        snapshot.target.controllerVerification.inProgress = false;
        snapshot.target.controllerVerification.invalidated = true;
        snapshot.target.controllerVerification.result = {};
        snapshot.target.controllerVerification.result.message =
            "The controller connection was not started because the endpoint "
            "fields are invalid.";
        RefreshPresentationEvidence(snapshot);
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Warning,
            "The controller connection was not started.",
            validation.endpointMessage);
        FinishProbe(cancellation);
        return;
    }

    const auto input = snapshot.target.input;
    if (validation.moduleAddressValid && validation.normalizedModuleAddress)
    {
        snapshot.target.selection = TunerTargetSelection{
            input,
            *validation.normalizedModuleAddress,
        };
    }
    else
    {
        snapshot.target.selection.reset();
    }
    snapshot.target.controllerVerification.inProgress = true;
    snapshot.target.controllerVerification.invalidated = false;
    snapshot.target.controllerVerification.probedEndpoint =
        ControllerEndpointForTarget(input);
    snapshot.target.controllerVerification.result = {};
    snapshot.target.controllerVerification.result.outcome =
        ControllerProbeOutcome::InProgress;
    snapshot.target.controllerVerification.result.message =
        "Connecting to the MVLC controller with read-only checks.";
    snapshot.target.verification.inProgress = false;
    snapshot.target.verification.invalidated = true;
    const auto savedBridgeFields = SaveBridgeConvenienceFields(
        storagePaths_, input);
    RefreshPresentationEvidence(snapshot);
    PublishStatus(
        std::move(snapshot),
        TunerStatusLevel::Information,
        "Connecting to the MVLC controller with read-only checks.",
        savedBridgeFields.success
            ? std::string{}
            : "The SSH destination and bridge command could not be saved: "
                + savedBridgeFields.message);

    const auto result = RunControllerProbe(
        transportFactory_,
        ControllerProbeRequest{MakeEndpointRequest(input)},
        *cancellation);

    snapshot = SnapshotCopy();
    snapshot.target.controllerVerification.inProgress = false;
    snapshot.target.controllerVerification.invalidated = false;
    snapshot.target.controllerVerification.probedEndpoint =
        ControllerEndpointForTarget(input);
    snapshot.target.controllerVerification.result = result;
    RefreshPresentationEvidence(snapshot);
    PublishStatus(
        std::move(snapshot),
        WithPersistenceWarning(
            ProbeStatusLevel(result.outcome),
            savedBridgeFields.success),
        result.message,
        savedBridgeFields.success
            ? std::string{}
            : "The SSH destination and bridge command could not be saved: "
                + savedBridgeFields.message);
    FinishProbe(cancellation);
}

void TuningSessionCoordinator::ProbeTarget(
    const std::shared_ptr<std::atomic<bool>>& cancellation)
{
    auto snapshot = SnapshotCopy();
    if (!ControllerVerificationIsFresh(snapshot.target))
    {
        snapshot.target.verification.inProgress = false;
        snapshot.target.verification.invalidated = true;
        snapshot.target.verification.result = {};
        snapshot.target.verification.result.message =
            "The target check requires a current verified controller "
            "connection.";
        RefreshPresentationEvidence(snapshot);
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Warning,
            "The read-only target check was not started.",
            "Run Connect successfully for the current endpoint first.");
        FinishProbe(cancellation);
        return;
    }

    const auto validation = ValidateTunerTargetInput(snapshot.target.input);
    if (!validation.moduleAddressValid || !validation.normalizedModuleAddress)
    {
        snapshot.target.selection.reset();
        snapshot.target.verification.inProgress = false;
        snapshot.target.verification.invalidated = true;
        snapshot.target.verification.result = {};
        snapshot.target.verification.result.message =
            "The target check requires a valid module address.";
        RefreshPresentationEvidence(snapshot);
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Warning,
            "The read-only target check was not started.",
            validation.moduleAddressMessage);
        FinishProbe(cancellation);
        return;
    }

    const auto input = snapshot.target.input;
    const TunerTargetSelection selected{
        input,
        *validation.normalizedModuleAddress,
    };
    snapshot.target.selection = selected;
    snapshot.target.verification.inProgress = true;
    snapshot.target.verification.invalidated = false;
    snapshot.target.verification.probedInput = input;
    snapshot.target.verification.result = {};
    snapshot.target.verification.result.outcome =
        TargetProbeOutcome::InProgress;
    snapshot.target.verification.result.message =
        "Checking the target module with read-only VME accesses.";
    snapshot.target.sessionGate = {};
    RefreshPresentationEvidence(snapshot);
    PublishStatus(
        std::move(snapshot),
        TunerStatusLevel::Information,
        "Checking the target module with read-only VME accesses.");

    const auto result = RunTargetProbe(
        transportFactory_,
        TargetProbeRequest{
            MakeEndpointRequest(input),
            selected.moduleAddress,
        },
        *cancellation);

    std::string persistenceError;
    if (result.outcome == TargetProbeOutcome::VerifiedIdle)
    {
        const ApplicationPreferences preferences{
            input.mvlcHost,
            input.moduleAddress,
            input.sshDestination,
            input.remoteBridgeCommand,
        };
        const auto saved = SaveApplicationPreferences(
            storagePaths_, preferences);
        if (!saved.success)
            persistenceError = saved.message;
    }

    snapshot = SnapshotCopy();
    snapshot.target.verification.inProgress = false;
    snapshot.target.verification.invalidated = false;
    snapshot.target.verification.probedInput = input;
    snapshot.target.verification.result = result;
    const bool controllerRevalidated =
        result.evidence.supportedControllerTypeAndFirmwareReverified
        && result.evidence.controllerDaqIdleReverified;
    if (!controllerRevalidated)
    {
        snapshot.target.controllerVerification.inProgress = false;
        snapshot.target.controllerVerification.invalidated = true;
    }
    RefreshPresentationEvidence(snapshot);
    PublishStatus(
        std::move(snapshot),
        persistenceError.empty()
            ? ProbeStatusLevel(result.outcome)
            : TunerStatusLevel::Warning,
        result.message,
        persistenceError.empty()
            ? std::string{}
            : "Target verification succeeded, but the verified Ethernet "
              "host and "
              "module address and the SSH convenience fields could not be "
              "saved: " + persistenceError);
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
            "Run Connect and Check successfully for the current fields "
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
            == TargetProbeOutcome::Cancelled
        || snapshot.target.controllerVerification.result.outcome
            == ControllerProbeOutcome::Cancelled;
    snapshot.target = {};
    RefreshWorkspaceTargetEvidence(snapshot);
    RefreshPresentationEvidence(snapshot);
    PublishStatus(
        std::move(snapshot),
        TunerStatusLevel::Information,
        cancelledProbe
            ? "The read-only connection or target check was cancelled and "
              "the tuner target was cleared."
            : "The tuner target was cleared.");
}

void TuningSessionCoordinator::SetWorkspace(
    SetTunerWorkspaceCommand command)
{
    auto snapshot = SnapshotCopy();
    snapshot.workspace = {};
    snapshot.workspace.outcome = TunerWorkspaceLoadOutcome::Loading;
    snapshot.workspace.sourcePath = std::move(command.sourcePath);
    snapshot.workspace.message = "Reading the optional MVME workspace.";
    snapshot.target.sessionGate = {};
    RefreshPresentationEvidence(snapshot);
    PublishStatus(
        std::move(snapshot),
        TunerStatusLevel::Information,
        "Reading the optional MVME workspace.");

    snapshot = SnapshotCopy();
    std::ifstream input(snapshot.workspace.sourcePath, std::ios::binary);
    if (!input.good())
    {
        snapshot.workspace.outcome =
            TunerWorkspaceLoadOutcome::FileUnavailable;
        snapshot.workspace.message =
            "The MVME workspace could not be opened for reading.";
        RefreshPresentationEvidence(snapshot);
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Error,
            "The optional MVME workspace was not loaded.",
            "Check the selected path and its read permissions.");
        return;
    }

    std::ostringstream contents;
    contents << input.rdbuf();
    if (input.bad())
    {
        snapshot.workspace.outcome =
            TunerWorkspaceLoadOutcome::FileUnavailable;
        snapshot.workspace.message =
            "Reading the MVME workspace failed before completion.";
        RefreshPresentationEvidence(snapshot);
        const auto detail = snapshot.workspace.message;
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Error,
            "The optional MVME workspace was not loaded.",
            detail);
        return;
    }

    auto parsed = ParseMvmeWorkspace(contents.str());
    if (!parsed.success || !parsed.workspace)
    {
        snapshot.workspace.outcome = TunerWorkspaceLoadOutcome::ParseFailed;
        snapshot.workspace.message = parsed.message.empty()
            ? "The MVME workspace is malformed or unsupported."
            : std::move(parsed.message);
        RefreshPresentationEvidence(snapshot);
        const auto detail = snapshot.workspace.message;
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Error,
            "The optional MVME workspace was not loaded.",
            detail);
        return;
    }

    snapshot.workspace.outcome = TunerWorkspaceLoadOutcome::Loaded;
    snapshot.workspace.workspace = std::move(parsed.workspace);
    RefreshWorkspaceTargetEvidence(snapshot);
    RefreshPresentationEvidence(snapshot);

    auto level = TunerStatusLevel::Success;
    if (!snapshot.workspace.evaluationPerformed
        || snapshot.workspace.evaluation.state
            != MvmeInitScriptEvaluationState::Complete)
    {
        level = TunerStatusLevel::Warning;
    }
    if (snapshot.workspace.evaluationPerformed
        && snapshot.workspace.evaluation.state
            == MvmeInitScriptEvaluationState::Failed)
    {
        level = TunerStatusLevel::Error;
    }
    const auto detail = snapshot.workspace.message;
    PublishStatus(
        std::move(snapshot),
        level,
        "The optional MVME workspace was loaded.",
        detail);
}

void TuningSessionCoordinator::ClearWorkspace()
{
    auto snapshot = SnapshotCopy();
    snapshot.workspace = {};
    snapshot.target.sessionGate = {};
    RefreshPresentationEvidence(snapshot);
    PublishStatus(
        std::move(snapshot),
        TunerStatusLevel::Information,
        "The optional MVME workspace was cleared.",
        "Controller and target verification were not changed.");
}

void TuningSessionCoordinator::RefreshWorkspaceTargetEvidence(
    TunerSnapshot& snapshot)
{
    auto& state = snapshot.workspace;
    state.targetLookupPerformed = false;
    state.evaluatedTargetAddress.reset();
    state.targetLookup = {};
    state.evaluationPerformed = false;
    state.evaluation = {};
    state.warnings.clear();

    if (state.outcome != TunerWorkspaceLoadOutcome::Loaded
        || !state.workspace)
    {
        return;
    }

    const auto validation = ValidateTunerTargetInput(snapshot.target.input);
    if (!validation.moduleAddressValid
        || !validation.normalizedModuleAddress)
    {
        state.message =
            "The workspace is parsed, but target lookup requires a valid "
            "module address.";
        return;
    }

    state.targetLookupPerformed = true;
    state.evaluatedTargetAddress = validation.normalizedModuleAddress;
    state.targetLookup = state.workspace->FindEnabledMdpp32ScpTarget(
        *validation.normalizedModuleAddress);
    if (state.targetLookup.status != MvmeWorkspaceTargetStatus::Found
        || !state.targetLookup.target)
    {
        state.message = state.targetLookup.message;
        return;
    }

    state.evaluationPerformed = true;
    state.evaluation = EvaluateMvmeTargetInitScripts(
        *state.workspace, *state.targetLookup.target);
    state.message = state.evaluation.message;
    for (const auto& unresolved : state.evaluation.unresolvedStatements)
    {
        state.warnings.push_back(
            "Script " + std::to_string(unresolved.location.scriptIndex)
            + ", line " + std::to_string(unresolved.location.lineNumber)
            + ": " + unresolved.message);
    }
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
        validation.endpointValid;
    snapshot.tuningSession.evidence.targetModuleAddressValid =
        validation.moduleAddressValid;
    snapshot.tuningSession.evidence.endpointEditingAllowed =
        !snapshot.target.controllerVerification.inProgress
        && !snapshot.target.verification.inProgress
        && snapshot.workspace.outcome != TunerWorkspaceLoadOutcome::Loading;
    snapshot.tuningSession.evidence.operationIdle =
        !snapshot.target.controllerVerification.inProgress
        && !snapshot.target.verification.inProgress
        && snapshot.workspace.outcome != TunerWorkspaceLoadOutcome::Loading
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
                        .noVmeOrModuleSettingWritesSent));
    }
}

} // namespace fidget
