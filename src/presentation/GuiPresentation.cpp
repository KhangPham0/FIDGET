#include "presentation/GuiPresentation.h"

#include <cstdio>

namespace fidget {
namespace {

using Evidence = TuningSessionEvidence;

constexpr std::size_t Index(const GuiAction action) noexcept
{
    return static_cast<std::size_t>(action);
}

void Allow(GuiActionSet& actions, const GuiAction action) noexcept
{
    actions.set(Index(action));
}

bool ControllerEndpointVerified(const Evidence& evidence) noexcept
{
    return evidence.controllerConnected
        && evidence.controllerIdentityVerified
        && evidence.controllerIdleVerified
        && evidence.controllerVerificationFresh;
}

bool TargetModuleVerified(const Evidence& evidence) noexcept
{
    return ControllerEndpointVerified(evidence)
        && evidence.targetIdentityAndFirmwareVerified
        && evidence.targetAcquisitionStoppedVerified
        && evidence.targetVerificationFresh
        && evidence.connectionVerificationFresh;
}

bool ControllerReady(const Evidence& evidence) noexcept
{
    return TargetModuleVerified(evidence);
}

bool ControlHeldAndVerified(const Evidence& evidence) noexcept
{
    return evidence.controlHeld
        && evidence.ownershipFingerprintVerified
        && !evidence.controllerReleased
        && !evidence.recoveryControllerReleased
        && !evidence.activeControllerUseDetected
        && !evidence.ownershipUncertain;
}

bool PreparationComplete(const Evidence& evidence) noexcept
{
    return ControllerReady(evidence)
        && evidence.noRecoveryPending
        && ControlHeldAndVerified(evidence)
        && evidence.liveRestoreSnapshotCaptured
        && evidence.recoveryRecordDurable
        && evidence.workspaceStartingSettingsResolved
        && evidence.acquisitionPrepared;
}

bool LatchReady(const Evidence& evidence) noexcept
{
    return PreparationComplete(evidence)
        && evidence.acquisitionLatchReady;
}

bool CurrentOwnershipAvailable(const Evidence& evidence) noexcept
{
    return ControlHeldAndVerified(evidence)
        && evidence.operationIdle
        && !evidence.controlsFrozen
        && !evidence.restorationInProgress
        && !evidence.ownershipUncertain;
}

bool RestorationCanStart(const Evidence& evidence) noexcept
{
    return ControlHeldAndVerified(evidence)
        && !evidence.controlsFrozen
        && evidence.liveRestoreSnapshotCaptured
        && evidence.recoveryRecordDurable
        && evidence.restorationRequired
        && evidence.cancelAndRestoreAvailable;
}

bool RestorationInProgressVerified(const Evidence& evidence) noexcept
{
    return ControlHeldAndVerified(evidence)
        && evidence.restorationRequired
        && evidence.restorationInProgress
        && evidence.liveRestoreSnapshotCaptured
        && evidence.recoveryRecordDurable;
}

bool GroupRestorationVerified(const Evidence& evidence) noexcept
{
    return evidence.groupCapturedStateRestored
        && evidence.groupRestorationReadbackVerified;
}

bool SessionRestorationVerified(const Evidence& evidence) noexcept
{
    return evidence.sessionCapturedStateRestored
        && evidence.sessionRestorationReadbackVerified;
}

bool FinishedVerified(const Evidence& evidence) noexcept
{
    return SessionRestorationVerified(evidence)
        && evidence.controllerReleased
        && (evidence.outputCreated
            || evidence.outputDeliberatelyDeclined);
}

bool ResultExitCanStart(const Evidence& evidence) noexcept
{
    if (!evidence.groupResultComplete
        || !evidence.resultExitAvailable)
    {
        return false;
    }

    return evidence.restorationRequired
        ? RestorationCanStart(evidence)
        : GroupRestorationVerified(evidence);
}

bool ConflictDetected(const Evidence& evidence) noexcept
{
    return evidence.activeControllerUseDetected;
}

bool ConflictMitigationsVerified(const Evidence& evidence) noexcept
{
    return evidence.activeControllerUseDetected
        && evidence.noControlTaken
        && evidence.noVmeOrModuleSettingWritesSent;
}

bool OwnershipUncertaintyDetected(const Evidence& evidence) noexcept
{
    return evidence.ownershipUncertain;
}

bool OwnershipUncertaintyMitigationsVerified(
    const Evidence& evidence) noexcept
{
    return evidence.ownershipUncertain
        && evidence.controlsFrozen
        && evidence.noBlindRollbackOrCleanupSent;
}

bool RecoveryRecordOutstanding(
    const TunerSnapshot& snapshot,
    const Evidence& evidence) noexcept
{
    return snapshot.recoveryRecordAvailable
        || ApplicationRecoveryBlocksNormalTuning(
            snapshot.applicationRecovery)
        || evidence.recoveryRecordPresent
        || snapshot.recoveryJournalStatus == RecoveryJournalStatus::Pending
        || snapshot.recoveryJournalStatus == RecoveryJournalStatus::Malformed;
}

bool RecoveryPending(
    const TunerSnapshot& snapshot,
    const Evidence& evidence) noexcept
{
    return RecoveryRecordOutstanding(snapshot, evidence)
        || evidence.recoveryContextEstablished
        || evidence.recoveryInProgress;
}

bool RecoveryCannotProceed(
    const TunerSnapshot& snapshot,
    const Evidence& evidence) noexcept
{
    const auto outcome = evidence.recoveryComparison;
    const bool cannotRestore =
        outcome == TuningRecoveryComparison::Unexpected
        || outcome == TuningRecoveryComparison::InsufficientEvidence;

    return RecoveryPending(snapshot, evidence)
        && (snapshot.applicationRecovery.state
                == ApplicationRecoveryDiscoveryState::Blocked
            || cannotRestore
            || snapshot.recoveryJournalStatus
                == RecoveryJournalStatus::Malformed);
}

bool RecoveryBlockedMitigationsVerified(
    const TunerSnapshot& snapshot,
    const Evidence& evidence) noexcept
{
    if (!RecoveryCannotProceed(snapshot, evidence)
        || !evidence.recoveryRecordRetained)
    {
        return false;
    }

    return evidence.recoveryComparison
            != TuningRecoveryComparison::Unexpected
        || evidence.unexpectedRecoveryValueNotOverwritten;
}

bool RecoveryValuesVerified(const Evidence& evidence) noexcept
{
    const bool comparisonVerified =
        (evidence.temporaryValuesRestored
         && evidence.recoveryComparison
             == TuningRecoveryComparison::LiveApplied)
        || (evidence.valuesAlreadyOriginal
            && evidence.recoveryComparison
                == TuningRecoveryComparison::LiveOriginal)
        || ((evidence.temporaryValuesRestored
             || evidence.valuesAlreadyOriginal)
            && evidence.recoveryComparison
                == TuningRecoveryComparison::VerifiedMixed);

    return evidence.recoveryControllerAndTargetIdentitiesVerified
        && evidence.recoveryStoppedStateVerified
        && comparisonVerified
        && evidence.recoveryReadbackVerified;
}

bool RecoveryComplete(
    const TunerSnapshot& snapshot,
    const Evidence& evidence) noexcept
{
    return evidence.recoveryContextEstablished
        && !evidence.recoveryInProgress
        && !RecoveryRecordOutstanding(snapshot, evidence)
        && evidence.noRecoveryPending
        && RecoveryValuesVerified(evidence)
        && evidence.recoveryJournalCleared
        && evidence.recoveryControllerReleased;
}

GuiEvidenceClaims MakeClaims(
    const TunerSnapshot& snapshot,
    const Evidence& evidence) noexcept
{
    GuiEvidenceClaims claims;
    claims.controllerEndpointVerified =
        ControllerEndpointVerified(evidence);
    claims.targetModuleVerified = TargetModuleVerified(evidence);
    claims.controllerReady = ControllerReady(evidence);
    claims.controlHeldAndVerified = ControlHeldAndVerified(evidence);
    claims.controllerAndTargetIdentitiesVerified =
        claims.controllerEndpointVerified
        && evidence.controllerIdentityVerified
        && evidence.targetIdentityAndFirmwareVerified
        && claims.targetModuleVerified;
    claims.liveRestoreSnapshotCaptured =
        evidence.liveRestoreSnapshotCaptured;
    claims.recoveryRecordDurable = evidence.recoveryRecordDurable;
    if (snapshot.applicationRecovery.state
        == ApplicationRecoveryDiscoveryState::PendingV5)
    {
        claims.recoveryRecordDurable = true;
    }
    claims.workspaceStartingSettingsResolved =
        evidence.workspaceStartingSettingsResolved;
    claims.acquisitionPrepared =
        evidence.liveRestoreSnapshotCaptured
        && evidence.recoveryRecordDurable
        && evidence.acquisitionPrepared;
    claims.acquisitionLatchReady =
        claims.acquisitionPrepared
        && evidence.acquisitionLatchReady;
    claims.currentAppliedValuesVerified =
        claims.controlHeldAndVerified
        && evidence.currentAppliedValuesVerified;
    claims.recommendationVerified =
        evidence.groupResultComplete
        && evidence.recommendationVerified;
    claims.settingChangesVerified =
        evidence.settingChangesVerified;
    claims.restorationInProgress =
        RestorationInProgressVerified(evidence);

    claims.groupCapturedStateRestored =
        GroupRestorationVerified(evidence);
    claims.groupRestorationReadbackVerified =
        GroupRestorationVerified(evidence);
    claims.recommendationRetained =
        GroupRestorationVerified(evidence)
        && evidence.recommendationRetained;
    claims.sessionCapturedStateRestored =
        SessionRestorationVerified(evidence);
    claims.sessionRestorationReadbackVerified =
        SessionRestorationVerified(evidence);
    claims.controllerReleased =
        SessionRestorationVerified(evidence)
        && evidence.controllerReleased;
    claims.outputCreatedOrDeclined =
        evidence.outputCreated
        || evidence.outputDeliberatelyDeclined;

    claims.noControlTaken =
        ConflictDetected(evidence)
        && evidence.noControlTaken;
    claims.noVmeOrModuleSettingWritesSent =
        ConflictDetected(evidence)
        && evidence.noVmeOrModuleSettingWritesSent;
    claims.controlsFrozen =
        OwnershipUncertaintyDetected(evidence)
        && evidence.controlsFrozen;
    claims.noBlindRollbackOrCleanupSent =
        OwnershipUncertaintyDetected(evidence)
        && evidence.noBlindRollbackOrCleanupSent;

    claims.recoveryIdentitiesVerified =
        evidence.recoveryControllerAndTargetIdentitiesVerified;
    claims.recoveryStoppedStateVerified =
        evidence.recoveryControllerAndTargetIdentitiesVerified
        && evidence.recoveryStoppedStateVerified;
    const bool unexpected =
        evidence.recoveryComparison
            == TuningRecoveryComparison::Unexpected;
    claims.unexpectedRecoveryValueNotOverwritten =
        unexpected
        && evidence.unexpectedRecoveryValueNotOverwritten;
    const bool applicationRecordRetained =
        ApplicationRecoveryHasRetainedEvidence(
            snapshot.applicationRecovery);
    claims.recoveryRecordRetained =
        evidence.recoveryRecordRetained || applicationRecordRetained;
    claims.noRecoveryWritesSent =
        evidence.recoveryRecordRetained
        && evidence.noRecoveryWritesSent;
    claims.recoveryValuesRestoredOrOriginal =
        RecoveryValuesVerified(evidence);
    claims.recoveryReadbackVerified =
        RecoveryValuesVerified(evidence);
    claims.recoveryJournalCleared =
        RecoveryValuesVerified(evidence)
        && !RecoveryRecordOutstanding(snapshot, evidence)
        && evidence.noRecoveryPending
        && evidence.recoveryJournalCleared;
    claims.recoveryControllerReleased =
        RecoveryComplete(snapshot, evidence);

    return claims;
}

GuiDrawer SelectDrawer(
    const GuiDrawer requested,
    const Evidence& evidence) noexcept
{
    switch (requested)
    {
    case GuiDrawer::None:
        return GuiDrawer::None;
    case GuiDrawer::Details:
        return evidence.detailsAvailable
            ? GuiDrawer::Details
            : GuiDrawer::None;
    case GuiDrawer::Logs:
        return evidence.logsAvailable
            ? GuiDrawer::Logs
            : GuiDrawer::None;
    case GuiDrawer::Help:
        return evidence.helpAvailable
            ? GuiDrawer::Help
            : GuiDrawer::None;
    case GuiDrawer::SettingChanges:
        return evidence.settingChangesVerified
            ? GuiDrawer::SettingChanges
            : GuiDrawer::None;
    case GuiDrawer::Count:
        return GuiDrawer::None;
    }

    return GuiDrawer::None;
}

GuiConflictRetry SelectConflictRetry(
    const TunerSnapshot& snapshot) noexcept
{
    if (TargetProbeEvidenceIsCurrent(snapshot.target))
    {
        switch (snapshot.target.verification.result.outcome)
        {
        case TargetProbeOutcome::ControllerDaqActive:
            return GuiConflictRetry::Connect;
        case TargetProbeOutcome::TargetAcquisitionActive:
            return GuiConflictRetry::Check;
        default:
            break;
        }
    }

    if (ControllerProbeEvidenceIsCurrent(snapshot.target)
        && snapshot.target.controllerVerification.result.outcome
            == ControllerProbeOutcome::ControllerDaqActive)
    {
        return GuiConflictRetry::Connect;
    }
    return GuiConflictRetry::None;
}

GuiPage SelectBaseNormalPage(
    const TuningSessionPhase phase,
    const Evidence& evidence,
    const GuiPresentationSelection& selection) noexcept
{
    const bool prepared = PreparationComplete(evidence);
    const bool latchReady = LatchReady(evidence);

    switch (phase)
    {
    case TuningSessionPhase::Home:
        if (selection.sshSettingsExpanded)
            return GuiPage::HomeSshExpanded;
        if (ControllerReady(evidence))
            return GuiPage::HomeReady;
        return GuiPage::HomeDisconnected;
    case TuningSessionPhase::Preparing:
        return GuiPage::Preparing;
    case TuningSessionPhase::Goal:
        return prepared ? GuiPage::Goal : GuiPage::Preparing;
    case TuningSessionPhase::Group:
        return prepared ? GuiPage::Group : GuiPage::Preparing;
    case TuningSessionPhase::AutomaticLearnSignal:
        if (evidence.moduleCouldNotBeArmed)
        {
            return RestorationInProgressVerified(evidence)
                ? GuiPage::Restoring
                : GuiPage::Preparing;
        }
        return latchReady
            ? GuiPage::AutomaticLearnSignal
            : GuiPage::Preparing;
    case TuningSessionPhase::AutomaticEnergy:
        if (!latchReady)
            return GuiPage::Preparing;
        return evidence.learnSignalComplete
            ? GuiPage::AutomaticEnergy
            : GuiPage::AutomaticLearnSignal;
    case TuningSessionPhase::AutomaticTiming:
        if (!latchReady)
            return GuiPage::Preparing;
        return evidence.learnSignalComplete
            ? GuiPage::AutomaticTiming
            : GuiPage::AutomaticLearnSignal;
    case TuningSessionPhase::Manual:
        if (!latchReady)
            return GuiPage::Preparing;
        return selection.manualPanel == GuiManualPanel::Timing
            ? GuiPage::ManualTiming
            : GuiPage::ManualEnergy;
    case TuningSessionPhase::GroupResult:
        if (!latchReady)
            return GuiPage::Preparing;
        return evidence.groupResultComplete
            ? GuiPage::GroupResult
            : GuiPage::AutomaticLearnSignal;
    case TuningSessionPhase::Restoring:
        return RestorationInProgressVerified(evidence)
            ? GuiPage::Restoring
            : GuiPage::Preparing;
    case TuningSessionPhase::NextGroup:
        return prepared
            && GroupRestorationVerified(evidence)
            && (evidence.recommendationRetained
                || evidence.noRecommendationAvailableVerified)
            ? GuiPage::NextGroup
            : RestorationInProgressVerified(evidence)
                ? GuiPage::Restoring
                : GuiPage::Preparing;
    case TuningSessionPhase::Finished:
        return FinishedVerified(evidence)
            ? GuiPage::Finished
            : RestorationInProgressVerified(evidence)
                ? GuiPage::Restoring
                : GuiPage::Preparing;
    }

    return GuiPage::HomeDisconnected;
}

bool SupportsChannelComparison(const GuiPage page) noexcept
{
    return page == GuiPage::AutomaticLearnSignal
        || page == GuiPage::AutomaticEnergy
        || page == GuiPage::AutomaticTiming
        || page == GuiPage::ManualEnergy
        || page == GuiPage::ManualTiming;
}

GuiPage SelectNormalPage(
    const TuningSessionPhase phase,
    const Evidence& evidence,
    const GuiPresentationSelection& selection) noexcept
{
    const auto page = SelectBaseNormalPage(phase, evidence, selection);
    return selection.compareChannels
            && evidence.comparisonAvailable
            && SupportsChannelComparison(page)
        ? GuiPage::CompareChannels
        : page;
}

GuiHeaderConnectionStatus SelectHeader(
    const TunerSnapshot& snapshot,
    const Evidence& evidence) noexcept
{
    if (RecoveryCannotProceed(snapshot, evidence))
        return GuiHeaderConnectionStatus::RecoveryBlocked;
    if (OwnershipUncertaintyDetected(evidence))
        return GuiHeaderConnectionStatus::OwnershipUncertain;
    if (RecoveryComplete(snapshot, evidence))
    {
        return evidence.controllerConnected
            ? GuiHeaderConnectionStatus::Connected
            : GuiHeaderConnectionStatus::Unknown;
    }
    if (RecoveryPending(snapshot, evidence))
    {
        return GuiHeaderConnectionStatus::RecoveryInProgress;
    }
    if (ConflictDetected(evidence))
        return GuiHeaderConnectionStatus::ControllerConflict;
    if (ControlHeldAndVerified(evidence)
        && !evidence.controllerReleased)
    {
        return GuiHeaderConnectionStatus::HasControl;
    }
    if (evidence.controllerConnected)
        return GuiHeaderConnectionStatus::Connected;
    if (snapshot.tuningSession.phase == TuningSessionPhase::Home)
        return GuiHeaderConnectionStatus::NotConnected;
    return GuiHeaderConnectionStatus::Unknown;
}

void AddDrawerActions(
    GuiViewState& view,
    const Evidence& evidence) noexcept
{
    if (evidence.detailsAvailable)
        Allow(view.allowedActions, GuiAction::OpenDetails);
    if (evidence.logsAvailable)
        Allow(view.allowedActions, GuiAction::OpenLogs);
    if (evidence.helpAvailable)
        Allow(view.allowedActions, GuiAction::OpenHelp);
    if (view.drawer != GuiDrawer::None)
        Allow(view.allowedActions, GuiAction::CloseDrawer);
}

void AddNavigationActions(
    GuiViewState& view,
    const Evidence& evidence) noexcept
{
    if (!evidence.primaryNavigationAvailable)
        return;

    switch (view.page)
    {
    case GuiPage::Restoring:
    case GuiPage::Finished:
    case GuiPage::ControllerConflict:
    case GuiPage::OwnershipUncertain:
    case GuiPage::Recovery:
    case GuiPage::RecoveryBlocked:
    case GuiPage::Recovered:
        return;
    default:
        break;
    }

    if (evidence.controlHeld
        || evidence.restorationRequired
        || evidence.restorationInProgress)
    {
        if (RestorationCanStart(evidence))
            Allow(view.allowedActions, GuiAction::NavigateHome);
        return;
    }

    if (!evidence.navigationAwayVerifiedSafe)
        return;

    Allow(view.allowedActions, GuiAction::NavigateHome);
    if (ControllerReady(evidence))
    {
        Allow(view.allowedActions, GuiAction::NavigateTune);
    }
}

void AddHomeActions(
    GuiViewState& view,
    const Evidence& evidence,
    const GuiPresentationSelection& selection) noexcept
{
    if (evidence.endpointInputsValid && evidence.operationIdle)
        Allow(view.allowedActions, GuiAction::Connect);
    if (ControllerEndpointVerified(evidence)
        && evidence.targetModuleAddressValid
        && evidence.operationIdle)
    {
        Allow(view.allowedActions, GuiAction::Check);
    }
    if (evidence.endpointEditingAllowed && evidence.operationIdle)
    {
        Allow(
            view.allowedActions,
            selection.sshSettingsExpanded
                ? GuiAction::CollapseSshSettings
                : GuiAction::ExpandSshSettings);
    }
    if (ControllerReady(evidence)
        && evidence.noRecoveryPending
        && evidence.operationIdle)
    {
        Allow(view.allowedActions, GuiAction::StartTuning);
    }
}

void AddManualActions(
    GuiViewState& view,
    const Evidence& evidence,
    const bool offerComparison) noexcept
{
    if (evidence.manualControlsAvailable
        && CurrentOwnershipAvailable(evidence))
    {
        Allow(view.allowedActions, GuiAction::SelectEnergyControls);
        Allow(view.allowedActions, GuiAction::SelectTimingControls);
        if (evidence.manualEditValid
            && evidence.currentAppliedValuesVerified)
        {
            Allow(view.allowedActions, GuiAction::CommitManualEdit);
        }
        if (evidence.undoAvailable)
            Allow(view.allowedActions, GuiAction::UndoManualChange);
        if (evidence.redoAvailable)
            Allow(view.allowedActions, GuiAction::RedoManualChange);
        if (evidence.workingDiffersFromOriginal)
            Allow(view.allowedActions, GuiAction::ReturnToOriginal);
        if (evidence.currentAppliedValuesVerified)
            Allow(view.allowedActions, GuiAction::FinishManual);
    }
    if (offerComparison && evidence.comparisonAvailable)
        Allow(view.allowedActions, GuiAction::CompareChannels);
}

void AddPageActions(
    GuiViewState& view,
    const TunerSnapshot& snapshot,
    const Evidence& evidence,
    const GuiPresentationSelection& selection) noexcept
{
    const bool operationIdle = evidence.operationIdle;
    const bool currentOwnership = CurrentOwnershipAvailable(evidence);
    const bool prepared = PreparationComplete(evidence);
    const bool canRestore = RestorationCanStart(evidence);

    switch (view.page)
    {
    case GuiPage::HomeDisconnected:
    case GuiPage::HomeSshExpanded:
    case GuiPage::HomeReady:
        AddHomeActions(view, evidence, selection);
        break;
    case GuiPage::Preparing:
        if (canRestore)
            Allow(view.allowedActions, GuiAction::StopAndRestore);
        break;
    case GuiPage::Goal:
        if (prepared && currentOwnership)
        {
            Allow(view.allowedActions, GuiAction::ChooseEnergy);
            Allow(view.allowedActions, GuiAction::ChooseTiming);
            Allow(view.allowedActions, GuiAction::ChooseBoth);
            Allow(view.allowedActions, GuiAction::ChooseManual);
        }
        if (canRestore)
            Allow(view.allowedActions, GuiAction::StopAndRestore);
        break;
    case GuiPage::Group:
    case GuiPage::NextGroup:
    {
        const bool selectionValid = evidence.goalSelected
            && evidence.groupSelectionValid
            && evidence.channelSelectionValid
            && currentOwnership;
        if (selectionValid && evidence.automaticPathSelected)
            Allow(view.allowedActions, GuiAction::BeginAutomatic);
        if (selectionValid && evidence.manualPathSelected)
            Allow(view.allowedActions, GuiAction::BeginManual);
        if (canRestore)
            Allow(view.allowedActions, GuiAction::StopAndRestore);
        break;
    }
    case GuiPage::AutomaticLearnSignal:
    case GuiPage::AutomaticEnergy:
    case GuiPage::AutomaticTiming:
        if (evidence.comparisonAvailable)
            Allow(view.allowedActions, GuiAction::CompareChannels);
        if (canRestore)
            Allow(view.allowedActions, GuiAction::StopAndRestore);
        break;
    case GuiPage::ManualEnergy:
    case GuiPage::ManualTiming:
        AddManualActions(view, evidence, true);
        if (canRestore)
            Allow(view.allowedActions, GuiAction::StopAndRestore);
        break;
    case GuiPage::CompareChannels:
        if (snapshot.tuningSession.phase == TuningSessionPhase::Manual)
            AddManualActions(view, evidence, false);
        if (evidence.comparisonAvailable)
            Allow(view.allowedActions, GuiAction::ReturnFromComparison);
        if (canRestore)
            Allow(view.allowedActions, GuiAction::StopAndRestore);
        break;
    case GuiPage::GroupResult:
        if (evidence.settingChangesVerified)
            Allow(view.allowedActions, GuiAction::ReviewSettingChanges);
        if (ResultExitCanStart(evidence))
        {
            Allow(view.allowedActions, GuiAction::TuneAnotherGroup);
            Allow(view.allowedActions, GuiAction::SaveAndFinish);
        }
        if (canRestore)
            Allow(view.allowedActions, GuiAction::StopAndRestore);
        break;
    case GuiPage::Restoring:
        break;
    case GuiPage::Finished:
        if (FinishedVerified(evidence))
            Allow(view.allowedActions, GuiAction::ReturnHome);
        break;
    case GuiPage::ControllerConflict:
        if (ConflictMitigationsVerified(evidence)
            && evidence.currentConnectionRequestValid
            && operationIdle)
        {
            Allow(view.allowedActions, GuiAction::CheckAgain);
        }
        if (ConflictMitigationsVerified(evidence)
            && evidence.noRecoveryPending
            && evidence.navigationAwayVerifiedSafe)
        {
            Allow(view.allowedActions, GuiAction::ReturnHome);
        }
        break;
    case GuiPage::OwnershipUncertain:
        if (OwnershipUncertaintyMitigationsVerified(evidence)
            && evidence.reconnectAvailable
            && evidence.recoveryRecordDurable
            && operationIdle)
        {
            Allow(view.allowedActions, GuiAction::ReconnectAndVerify);
        }
        break;
    case GuiPage::Recovery:
        break;
    case GuiPage::RecoveryBlocked:
        if (RecoveryBlockedMitigationsVerified(snapshot, evidence)
            && evidence.guardedRecoveryRetryAvailable
            && operationIdle)
        {
            Allow(view.allowedActions, GuiAction::RetryVerification);
        }
        break;
    case GuiPage::Recovered:
        if (RecoveryComplete(snapshot, evidence))
            Allow(view.allowedActions, GuiAction::ContinueToHome);
        break;
    case GuiPage::Count:
        break;
    }
}

} // namespace

bool Allows(const GuiActionSet& actions, const GuiAction action) noexcept
{
    const auto index = Index(action);
    return index < actions.size() && actions.test(index);
}

GuiViewState PresentGui(
    const TunerSnapshot& snapshot,
    const GuiPresentationSelection& selection)
{
    const auto& evidence = snapshot.tuningSession.evidence;

    GuiViewState view;
    if (snapshot.target.selection.has_value()
        && snapshot.target.selection->input == snapshot.target.input)
    {
        view.targetModuleAddressA32 =
            snapshot.target.selection->moduleAddress.FullA32Value();
    }
    view.applicationRecovery.state = snapshot.applicationRecovery.state;
    view.applicationRecovery.blockReason =
        snapshot.applicationRecovery.blockReason;
    view.applicationRecovery.message = snapshot.applicationRecovery.message;
    if (snapshot.applicationRecovery.state
            == ApplicationRecoveryDiscoveryState::PendingV5
        && snapshot.applicationRecovery.record.has_value()
        && snapshot.applicationRecovery.record->version5.has_value())
    {
        view.applicationRecovery.endpoint =
            snapshot.applicationRecovery.record->version5->endpoint;
        view.applicationRecovery.identity =
            snapshot.applicationRecovery.record->version5->identity;
    }
    view.claims = MakeClaims(snapshot, evidence);
    view.drawer = SelectDrawer(selection.drawer, evidence);
    view.headerConnection = SelectHeader(snapshot, evidence);
    view.conflictRetry = SelectConflictRetry(snapshot);
    if (evidence.moduleCouldNotBeArmed)
        view.tuningOutcome = GuiTuningOutcome::ModuleCouldNotBeArmed;
    else if (evidence.insufficientData)
        view.tuningOutcome = GuiTuningOutcome::InsufficientData;

    if (RecoveryCannotProceed(snapshot, evidence))
        view.page = GuiPage::RecoveryBlocked;
    else if (OwnershipUncertaintyDetected(evidence))
        view.page = GuiPage::OwnershipUncertain;
    else if (RecoveryComplete(snapshot, evidence))
        view.page = GuiPage::Recovered;
    else if (RecoveryPending(snapshot, evidence))
        view.page = GuiPage::Recovery;
    else if (ConflictDetected(evidence))
        view.page = GuiPage::ControllerConflict;
    else if (evidence.moduleCouldNotBeArmed)
    {
        view.page = RestorationInProgressVerified(evidence)
            ? GuiPage::Restoring
            : GuiPage::Preparing;
    }
    else if (RestorationInProgressVerified(evidence))
        view.page = GuiPage::Restoring;
    else
        view.page = SelectNormalPage(
            snapshot.tuningSession.phase,
            evidence,
            selection);

    AddDrawerActions(view, evidence);
    AddNavigationActions(view, evidence);
    AddPageActions(view, snapshot, evidence, selection);
    return view;
}

const char* GuiHeaderConnectionStatusText(
    const GuiHeaderConnectionStatus status) noexcept
{
    switch (status)
    {
    case GuiHeaderConnectionStatus::Unknown:
        return "Controller status unknown";
    case GuiHeaderConnectionStatus::NotConnected:
        return "Not connected";
    case GuiHeaderConnectionStatus::Connected:
        return "Connected";
    case GuiHeaderConnectionStatus::HasControl:
        return "FIDGET has control";
    case GuiHeaderConnectionStatus::ControllerConflict:
        return "Controller is active elsewhere";
    case GuiHeaderConnectionStatus::OwnershipUncertain:
        return "Connection lost \u00b7 ownership uncertain";
    case GuiHeaderConnectionStatus::RecoveryInProgress:
        return "Recovery in progress";
    case GuiHeaderConnectionStatus::RecoveryBlocked:
        return "Recovery blocked";
    case GuiHeaderConnectionStatus::Count:
        return "Controller status unknown";
    }

    return "Controller status unknown";
}

std::string GuiTargetAddressText(
    const std::uint32_t fullA32Address,
    const GuiTargetAddressDisplay display)
{
    char buffer[16]{};
    if (display == GuiTargetAddressDisplay::HomeMvmeShorthand)
    {
        std::snprintf(
            buffer,
            sizeof(buffer),
            "0x%04X",
            static_cast<unsigned>(fullA32Address >> 16U));
    }
    else
    {
        std::snprintf(
            buffer,
            sizeof(buffer),
            "0x%08X",
            static_cast<unsigned>(fullA32Address));
    }
    return buffer;
}

} // namespace fidget
