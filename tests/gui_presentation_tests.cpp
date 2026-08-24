#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "presentation/GuiPresentation.h"

#include <array>
#include <initializer_list>
#include <string>
#include <vector>

namespace {

using namespace fidget;

GuiActionSet Actions(const std::initializer_list<GuiAction> values)
{
    GuiActionSet result;
    for (const auto value: values)
        result.set(static_cast<std::size_t>(value));
    return result;
}

TunerSnapshot ReadySnapshot()
{
    TunerSnapshot snapshot;
    auto& evidence = snapshot.tuningSession.evidence;
    evidence.endpointInputsValid = true;
    evidence.endpointEditingAllowed = true;
    evidence.currentConnectionRequestValid = true;
    evidence.controllerConnected = true;
    evidence.controllerIdentityVerified = true;
    evidence.targetIdentityAndFirmwareVerified = true;
    evidence.controllerIdleVerified = true;
    evidence.connectionVerificationFresh = true;
    evidence.noRecoveryPending = true;
    evidence.operationIdle = true;
    evidence.navigationAwayVerifiedSafe = true;
    return snapshot;
}

TunerSnapshot PreparedSnapshot(const TuningSessionPhase phase)
{
    auto snapshot = ReadySnapshot();
    snapshot.tuningSession.phase = phase;
    auto& evidence = snapshot.tuningSession.evidence;
    evidence.controlHeld = true;
    evidence.ownershipFingerprintVerified = true;
    evidence.liveRestoreSnapshotCaptured = true;
    evidence.recoveryRecordDurable = true;
    evidence.workspaceStartingSettingsResolved = true;
    evidence.acquisitionPrepared = true;
    evidence.restorationRequired = true;
    evidence.cancelAndRestoreAvailable = true;
    return snapshot;
}

TunerSnapshot LatchedSnapshot(const TuningSessionPhase phase)
{
    auto snapshot = PreparedSnapshot(phase);
    snapshot.tuningSession.evidence.acquisitionLatchReady = true;
    return snapshot;
}

TunerSnapshot RecoverySnapshot()
{
    auto snapshot = ReadySnapshot();
    snapshot.recoveryRecordAvailable = true;
    auto& evidence = snapshot.tuningSession.evidence;
    evidence.noRecoveryPending = false;
    evidence.recoveryRecordPresent = true;
    evidence.recoveryInProgress = true;
    return snapshot;
}

struct PageCase
{
    const char* name;
    TunerSnapshot snapshot;
    GuiPresentationSelection selection;
    GuiPage page;
    GuiHeaderConnectionStatus header;
    GuiActionSet actions;
};

} // namespace

TEST_CASE("the presenter covers every approved primary page")
{
    auto homeDisconnected = TunerSnapshot{};
    {
        auto& evidence = homeDisconnected.tuningSession.evidence;
        evidence.endpointInputsValid = true;
        evidence.endpointEditingAllowed = true;
        evidence.noRecoveryPending = true;
        evidence.operationIdle = true;
    }

    const auto homeReady = ReadySnapshot();

    auto preparing = ReadySnapshot();
    preparing.tuningSession.phase = TuningSessionPhase::Preparing;
    preparing.tuningSession.evidence.controlHeld = true;
    preparing.tuningSession.evidence.ownershipFingerprintVerified = true;

    auto goal = PreparedSnapshot(TuningSessionPhase::Goal);

    auto group = PreparedSnapshot(TuningSessionPhase::Group);
    group.tuningSession.evidence.goalSelected = true;
    group.tuningSession.evidence.automaticPathSelected = true;
    group.tuningSession.evidence.groupSelectionValid = true;
    group.tuningSession.evidence.channelSelectionValid = true;

    auto learn = LatchedSnapshot(TuningSessionPhase::AutomaticLearnSignal);

    auto energy = LatchedSnapshot(TuningSessionPhase::AutomaticEnergy);
    energy.tuningSession.evidence.learnSignalComplete = true;
    energy.tuningSession.evidence.currentAppliedValuesVerified = true;

    auto timing = LatchedSnapshot(TuningSessionPhase::AutomaticTiming);
    timing.tuningSession.evidence.learnSignalComplete = true;
    timing.tuningSession.evidence.currentAppliedValuesVerified = true;

    auto manualEnergy = LatchedSnapshot(TuningSessionPhase::Manual);
    {
        auto& evidence = manualEnergy.tuningSession.evidence;
        evidence.manualControlsAvailable = true;
        evidence.manualEditValid = true;
        evidence.currentAppliedValuesVerified = true;
        evidence.undoAvailable = true;
        evidence.redoAvailable = true;
        evidence.workingDiffersFromOriginal = true;
        evidence.comparisonAvailable = true;
    }

    auto manualTiming = LatchedSnapshot(TuningSessionPhase::Manual);
    manualTiming.tuningSession.evidence.manualControlsAvailable = true;
    manualTiming.tuningSession.evidence.currentAppliedValuesVerified = true;

    auto compare = LatchedSnapshot(TuningSessionPhase::Manual);
    compare.tuningSession.evidence.manualControlsAvailable = true;
    compare.tuningSession.evidence.currentAppliedValuesVerified = true;
    compare.tuningSession.evidence.comparisonAvailable = true;

    auto result = LatchedSnapshot(TuningSessionPhase::GroupResult);
    result.tuningSession.evidence.groupResultComplete = true;
    result.tuningSession.evidence.recommendationVerified = true;
    result.tuningSession.evidence.settingChangesVerified = true;
    result.tuningSession.evidence.resultExitAvailable = true;

    auto restoring = PreparedSnapshot(TuningSessionPhase::Restoring);
    restoring.tuningSession.evidence.restorationInProgress = true;

    auto nextGroup = PreparedSnapshot(TuningSessionPhase::NextGroup);
    {
        auto& evidence = nextGroup.tuningSession.evidence;
        evidence.groupCapturedStateRestored = true;
        evidence.groupRestorationReadbackVerified = true;
        evidence.recommendationRetained = true;
        evidence.goalSelected = true;
        evidence.automaticPathSelected = true;
        evidence.groupSelectionValid = true;
        evidence.channelSelectionValid = true;
    }

    auto finished = ReadySnapshot();
    finished.tuningSession.phase = TuningSessionPhase::Finished;
    finished.tuningSession.evidence.sessionCapturedStateRestored = true;
    finished.tuningSession.evidence.sessionRestorationReadbackVerified = true;
    finished.tuningSession.evidence.controllerReleased = true;
    finished.tuningSession.evidence.outputCreated = true;

    auto conflict = ReadySnapshot();
    conflict.tuningSession.evidence.activeControllerUseDetected = true;
    conflict.tuningSession.evidence.noControlTaken = true;
    conflict.tuningSession.evidence.noStateChangingCommandsSent = true;

    auto uncertain = PreparedSnapshot(TuningSessionPhase::AutomaticEnergy);
    uncertain.tuningSession.evidence.ownershipUncertain = true;
    uncertain.tuningSession.evidence.controlsFrozen = true;
    uncertain.tuningSession.evidence.noBlindRollbackOrCleanupSent = true;
    uncertain.tuningSession.evidence.reconnectAvailable = true;
    uncertain.tuningSession.evidence.logsAvailable = true;

    auto recovery = RecoverySnapshot();

    auto recoveryBlocked = RecoverySnapshot();
    recoveryBlocked.tuningSession.evidence.recoveryComparison =
        TuningRecoveryComparison::Unexpected;
    recoveryBlocked.tuningSession.evidence
        .unexpectedRecoveryValueNotOverwritten = true;
    recoveryBlocked.tuningSession.evidence.recoveryRecordRetained = true;
    recoveryBlocked.tuningSession.evidence.noRecoveryWritesSent = true;
    recoveryBlocked.tuningSession.evidence
        .guardedRecoveryRetryAvailable = true;
    recoveryBlocked.tuningSession.evidence.detailsAvailable = true;

    auto recovered = ReadySnapshot();
    recovered.tuningSession.evidence.recoveryContextEstablished = true;
    recovered.tuningSession.evidence
        .recoveryControllerAndTargetIdentitiesVerified = true;
    recovered.tuningSession.evidence.recoveryStoppedStateVerified = true;
    recovered.tuningSession.evidence.recoveryComparison =
        TuningRecoveryComparison::LiveApplied;
    recovered.tuningSession.evidence.temporaryValuesRestored = true;
    recovered.tuningSession.evidence.recoveryReadbackVerified = true;
    recovered.tuningSession.evidence.recoveryJournalCleared = true;
    recovered.tuningSession.evidence.recoveryControllerReleased = true;

    const std::vector<PageCase> cases = {
        {"home disconnected", homeDisconnected, {},
         GuiPage::HomeDisconnected,
         GuiHeaderConnectionStatus::NotConnected,
         Actions({GuiAction::Connect, GuiAction::ExpandSshSettings})},
        {"home ready", homeReady, {}, GuiPage::HomeReady,
         GuiHeaderConnectionStatus::Connected,
         Actions({GuiAction::Connect, GuiAction::Check,
                  GuiAction::StartTuning,
                  GuiAction::ExpandSshSettings})},
        {"preparing", preparing, {}, GuiPage::Preparing,
         GuiHeaderConnectionStatus::HasControl, {}},
        {"goal", goal, {}, GuiPage::Goal,
         GuiHeaderConnectionStatus::HasControl,
         Actions({GuiAction::ChooseEnergy, GuiAction::ChooseTiming,
                  GuiAction::ChooseBoth, GuiAction::ChooseManual,
                  GuiAction::StopAndRestore})},
        {"group", group, {}, GuiPage::Group,
         GuiHeaderConnectionStatus::HasControl,
         Actions({GuiAction::BeginAutomatic,
                  GuiAction::StopAndRestore})},
        {"learn signal", learn, {}, GuiPage::AutomaticLearnSignal,
         GuiHeaderConnectionStatus::HasControl,
         Actions({GuiAction::StopAndRestore})},
        {"automatic energy", energy, {}, GuiPage::AutomaticEnergy,
         GuiHeaderConnectionStatus::HasControl,
         Actions({GuiAction::StopAndRestore})},
        {"automatic timing", timing, {}, GuiPage::AutomaticTiming,
         GuiHeaderConnectionStatus::HasControl,
         Actions({GuiAction::StopAndRestore})},
        {"manual energy", manualEnergy, {}, GuiPage::ManualEnergy,
         GuiHeaderConnectionStatus::HasControl,
         Actions({GuiAction::SelectEnergyControls,
                  GuiAction::SelectTimingControls,
                  GuiAction::CommitManualEdit,
                  GuiAction::UndoManualChange,
                  GuiAction::RedoManualChange,
                  GuiAction::ReturnToOriginal,
                  GuiAction::FinishManual,
                  GuiAction::CompareChannels,
                  GuiAction::StopAndRestore})},
        {"manual timing", manualTiming,
         GuiPresentationSelection{
             GuiDrawer::None, false, GuiManualPanel::Timing, false},
         GuiPage::ManualTiming,
         GuiHeaderConnectionStatus::HasControl,
         Actions({GuiAction::SelectEnergyControls,
                  GuiAction::SelectTimingControls,
                  GuiAction::FinishManual,
                  GuiAction::StopAndRestore})},
        {"compare channels", compare,
         GuiPresentationSelection{
             GuiDrawer::None, false, GuiManualPanel::Energy, true},
         GuiPage::CompareChannels,
         GuiHeaderConnectionStatus::HasControl,
         Actions({GuiAction::SelectEnergyControls,
                  GuiAction::SelectTimingControls,
                  GuiAction::FinishManual,
                  GuiAction::ReturnFromComparison,
                  GuiAction::StopAndRestore})},
        {"group result", result, {}, GuiPage::GroupResult,
         GuiHeaderConnectionStatus::HasControl,
         Actions({GuiAction::ReviewSettingChanges,
                  GuiAction::TuneAnotherGroup,
                  GuiAction::SaveAndFinish,
                  GuiAction::StopAndRestore})},
        {"restoring", restoring, {}, GuiPage::Restoring,
         GuiHeaderConnectionStatus::HasControl, {}},
        {"next group", nextGroup, {}, GuiPage::NextGroup,
         GuiHeaderConnectionStatus::HasControl,
         Actions({GuiAction::BeginAutomatic,
                  GuiAction::StopAndRestore})},
        {"finished", finished, {}, GuiPage::Finished,
         GuiHeaderConnectionStatus::Connected,
         Actions({GuiAction::ReturnHome})},
        {"controller conflict", conflict, {},
         GuiPage::ControllerConflict,
         GuiHeaderConnectionStatus::ControllerConflict,
         Actions({GuiAction::CheckAgain, GuiAction::ReturnHome})},
        {"ownership uncertain", uncertain, {},
         GuiPage::OwnershipUncertain,
         GuiHeaderConnectionStatus::OwnershipUncertain,
         Actions({GuiAction::ReconnectAndVerify,
                  GuiAction::OpenLogs})},
        {"recovery", recovery, {}, GuiPage::Recovery,
         GuiHeaderConnectionStatus::RecoveryInProgress, {}},
        {"recovery blocked", recoveryBlocked, {},
         GuiPage::RecoveryBlocked,
         GuiHeaderConnectionStatus::RecoveryBlocked,
         Actions({GuiAction::RetryVerification,
                  GuiAction::OpenDetails})},
        {"recovered", recovered, {}, GuiPage::Recovered,
         GuiHeaderConnectionStatus::Connected,
         Actions({GuiAction::ContinueToHome})},
        {"home SSH expanded", homeDisconnected,
         GuiPresentationSelection{GuiDrawer::None, true},
         GuiPage::HomeSshExpanded,
         GuiHeaderConnectionStatus::NotConnected,
         Actions({GuiAction::Connect,
                  GuiAction::CollapseSshSettings})},
    };

    REQUIRE(cases.size() == GuiPageCount);
    std::array<bool, GuiPageCount> seen{};

    for (const auto& testCase: cases)
    {
        CAPTURE(testCase.name);
        const auto view = PresentGui(testCase.snapshot, testCase.selection);
        CHECK(view.page == testCase.page);
        CHECK(view.headerConnection == testCase.header);
        CHECK(view.allowedActions == testCase.actions);
        const auto index = static_cast<std::size_t>(view.page);
        REQUIRE(index < seen.size());
        CHECK_FALSE(seen[index]);
        seen[index] = true;
    }

    for (const auto value: seen)
        CHECK(value);
}

TEST_CASE("drawers are evidence-gated overlays on primary pages")
{
    auto active = LatchedSnapshot(TuningSessionPhase::AutomaticEnergy);
    active.tuningSession.evidence.learnSignalComplete = true;
    active.tuningSession.evidence.currentAppliedValuesVerified = true;
    active.tuningSession.evidence.detailsAvailable = true;
    active.tuningSession.evidence.logsAvailable = true;

    auto ready = ReadySnapshot();
    ready.tuningSession.evidence.helpAvailable = true;

    auto result = LatchedSnapshot(TuningSessionPhase::GroupResult);
    result.tuningSession.evidence.groupResultComplete = true;
    result.tuningSession.evidence.settingChangesVerified = true;

    struct DrawerCase
    {
        TunerSnapshot snapshot;
        GuiDrawer requested;
        GuiPage basePage;
    };

    const std::array<DrawerCase, GuiDrawerCount> cases = {{
        {active, GuiDrawer::None, GuiPage::AutomaticEnergy},
        {active, GuiDrawer::Details, GuiPage::AutomaticEnergy},
        {active, GuiDrawer::Logs, GuiPage::AutomaticEnergy},
        {ready, GuiDrawer::Help, GuiPage::HomeReady},
        {result, GuiDrawer::SettingChanges, GuiPage::GroupResult},
    }};

    for (const auto& testCase: cases)
    {
        const auto view = PresentGui(
            testCase.snapshot,
            GuiPresentationSelection{testCase.requested, false});
        CHECK(view.page == testCase.basePage);
        CHECK(view.drawer == testCase.requested);
        CHECK(Allows(view.allowedActions, GuiAction::CloseDrawer)
              == (testCase.requested != GuiDrawer::None));
    }

    const auto unavailable = PresentGui(
        ReadySnapshot(),
        GuiPresentationSelection{GuiDrawer::Details, false});
    CHECK(unavailable.drawer == GuiDrawer::None);
    CHECK_FALSE(Allows(unavailable.allowedActions, GuiAction::CloseDrawer));

    auto restoring = PreparedSnapshot(TuningSessionPhase::Restoring);
    auto& restoringEvidence = restoring.tuningSession.evidence;
    restoringEvidence.restorationInProgress = true;
    restoringEvidence.detailsAvailable = true;
    restoringEvidence.logsAvailable = true;
    restoringEvidence.helpAvailable = true;
    restoringEvidence.primaryNavigationAvailable = true;
    restoringEvidence.navigationAwayVerifiedSafe = true;
    const auto restoringView = PresentGui(restoring);
    CHECK(restoringView.page == GuiPage::Restoring);
    CHECK(restoringView.claims.restorationInProgress);
    CHECK(Allows(restoringView.allowedActions, GuiAction::OpenDetails));
    CHECK(Allows(restoringView.allowedActions, GuiAction::OpenLogs));
    CHECK(Allows(restoringView.allowedActions, GuiAction::OpenHelp));
    CHECK_FALSE(Allows(
        restoringView.allowedActions,
        GuiAction::NavigateHome));
    CHECK_FALSE(Allows(
        restoringView.allowedActions,
        GuiAction::NavigateTune));

    restoring.tuningSession.evidence.controlHeld = false;
    const auto uncontrolledRestoration = PresentGui(restoring);
    CHECK(uncontrolledRestoration.page == GuiPage::Preparing);
    CHECK_FALSE(uncontrolledRestoration.claims.restorationInProgress);

    auto staleManualPhase = LatchedSnapshot(TuningSessionPhase::Manual);
    auto& staleEvidence = staleManualPhase.tuningSession.evidence;
    staleEvidence.restorationInProgress = true;
    staleEvidence.manualControlsAvailable = true;
    staleEvidence.currentAppliedValuesVerified = true;
    staleEvidence.primaryNavigationAvailable = true;
    staleEvidence.navigationAwayVerifiedSafe = true;
    const auto stalePhaseView = PresentGui(staleManualPhase);
    CHECK(stalePhaseView.page == GuiPage::Restoring);
    CHECK(stalePhaseView.claims.restorationInProgress);
    CHECK_FALSE(Allows(
        stalePhaseView.allowedActions,
        GuiAction::CommitManualEdit));
    CHECK_FALSE(Allows(
        stalePhaseView.allowedActions,
        GuiAction::NavigateHome));
    CHECK_FALSE(Allows(
        stalePhaseView.allowedActions,
        GuiAction::NavigateTune));
}

TEST_CASE("header states use explicit evidence and safety precedence")
{
    auto unknown = TunerSnapshot{};
    unknown.tuningSession.phase = TuningSessionPhase::Preparing;

    auto connected = TunerSnapshot{};
    connected.tuningSession.evidence.controllerConnected = true;

    auto controlled = connected;
    controlled.tuningSession.phase = TuningSessionPhase::Preparing;
    controlled.tuningSession.evidence.controlHeld = true;
    controlled.tuningSession.evidence.ownershipFingerprintVerified = true;

    const std::array<GuiHeaderConnectionStatus, 8> statuses = {{
        PresentGui(unknown).headerConnection,
        PresentGui(TunerSnapshot{}).headerConnection,
        PresentGui(connected).headerConnection,
        PresentGui(controlled).headerConnection,
        GuiHeaderConnectionStatus::ControllerConflict,
        GuiHeaderConnectionStatus::OwnershipUncertain,
        GuiHeaderConnectionStatus::RecoveryInProgress,
        GuiHeaderConnectionStatus::RecoveryBlocked,
    }};

    CHECK(statuses[0] == GuiHeaderConnectionStatus::Unknown);
    CHECK(statuses[1] == GuiHeaderConnectionStatus::NotConnected);
    CHECK(statuses[2] == GuiHeaderConnectionStatus::Connected);
    CHECK(statuses[3] == GuiHeaderConnectionStatus::HasControl);
    const std::array<const char*, 8> expectedText = {{
        "Controller status unknown",
        "Not connected",
        "Connected",
        "FIDGET has control",
        "Controller is active elsewhere",
        "Connection lost \u00b7 ownership uncertain",
        "Recovery in progress",
        "Recovery blocked",
    }};
    for (std::size_t index = 0U; index < statuses.size(); ++index)
    {
        CHECK(std::string(GuiHeaderConnectionStatusText(statuses[index]))
              == expectedText[index]);
    }

    auto contradictory = RecoverySnapshot();
    auto& evidence = contradictory.tuningSession.evidence;
    evidence.recoveryComparison = TuningRecoveryComparison::Unexpected;
    evidence.unexpectedRecoveryValueNotOverwritten = true;
    evidence.recoveryRecordRetained = true;
    evidence.ownershipUncertain = true;
    evidence.controlsFrozen = true;
    evidence.noBlindRollbackOrCleanupSent = true;
    evidence.activeControllerUseDetected = true;
    evidence.noControlTaken = true;
    evidence.noStateChangingCommandsSent = true;

    CHECK(PresentGui(contradictory).page == GuiPage::RecoveryBlocked);
    CHECK(PresentGui(contradictory).headerConnection
          == GuiHeaderConnectionStatus::RecoveryBlocked);

    evidence.recoveryComparison = TuningRecoveryComparison::NotCompared;
    CHECK(PresentGui(contradictory).page == GuiPage::OwnershipUncertain);
    evidence.ownershipUncertain = false;
    CHECK(PresentGui(contradictory).page == GuiPage::Recovery);
    contradictory.recoveryRecordAvailable = false;
    evidence.recoveryRecordPresent = false;
    evidence.recoveryInProgress = false;
    CHECK(PresentGui(contradictory).page == GuiPage::ControllerConflict);
    evidence.activeControllerUseDetected = false;
    CHECK(PresentGui(contradictory).page == GuiPage::HomeReady);
}

TEST_CASE("a backend phase alone never grants an action")
{
    constexpr std::array<TuningSessionPhase, 12> phases = {{
        TuningSessionPhase::Home,
        TuningSessionPhase::Preparing,
        TuningSessionPhase::Goal,
        TuningSessionPhase::Group,
        TuningSessionPhase::AutomaticLearnSignal,
        TuningSessionPhase::AutomaticEnergy,
        TuningSessionPhase::AutomaticTiming,
        TuningSessionPhase::Manual,
        TuningSessionPhase::GroupResult,
        TuningSessionPhase::Restoring,
        TuningSessionPhase::NextGroup,
        TuningSessionPhase::Finished,
    }};

    for (const auto phase: phases)
    {
        TunerSnapshot snapshot;
        snapshot.tuningSession.phase = phase;
        CHECK(PresentGui(snapshot).allowedActions.none());
    }
}

TEST_CASE("ready and preparation actions require every decisive fact")
{
    using Member = bool TuningSessionEvidence::*;
    constexpr std::array<Member, 5> readyFacts = {{
        &TuningSessionEvidence::controllerConnected,
        &TuningSessionEvidence::controllerIdentityVerified,
        &TuningSessionEvidence::targetIdentityAndFirmwareVerified,
        &TuningSessionEvidence::controllerIdleVerified,
        &TuningSessionEvidence::connectionVerificationFresh,
    }};

    for (const auto member: readyFacts)
    {
        auto snapshot = ReadySnapshot();
        snapshot.tuningSession.evidence.*member = false;
        const auto view = PresentGui(snapshot);
        CHECK(view.page == GuiPage::HomeDisconnected);
        CHECK_FALSE(Allows(view.allowedActions, GuiAction::StartTuning));
        CHECK_FALSE(view.claims.controllerReady);
    }

    auto readyButBusy = ReadySnapshot();
    readyButBusy.tuningSession.evidence.operationIdle = false;
    CHECK(PresentGui(readyButBusy).page == GuiPage::HomeReady);
    CHECK_FALSE(Allows(
        PresentGui(readyButBusy).allowedActions,
        GuiAction::StartTuning));

    constexpr std::array<Member, 7> preparationFacts = {{
        &TuningSessionEvidence::noRecoveryPending,
        &TuningSessionEvidence::controlHeld,
        &TuningSessionEvidence::ownershipFingerprintVerified,
        &TuningSessionEvidence::liveRestoreSnapshotCaptured,
        &TuningSessionEvidence::recoveryRecordDurable,
        &TuningSessionEvidence::workspaceStartingSettingsResolved,
        &TuningSessionEvidence::acquisitionPrepared,
    }};

    const auto preparedView = PresentGui(
        PreparedSnapshot(TuningSessionPhase::Goal));
    CHECK(preparedView.claims.controllerAndTargetIdentitiesVerified);
    CHECK(preparedView.claims.liveRestoreSnapshotCaptured);
    CHECK(preparedView.claims.recoveryRecordDurable);
    CHECK(preparedView.claims.workspaceStartingSettingsResolved);
    CHECK(preparedView.claims.acquisitionPrepared);

    for (const auto member: preparationFacts)
    {
        auto snapshot = PreparedSnapshot(TuningSessionPhase::Goal);
        snapshot.tuningSession.evidence.*member = false;
        const auto view = PresentGui(snapshot);
        CHECK(view.page == GuiPage::Preparing);
        CHECK_FALSE(Allows(view.allowedActions, GuiAction::ChooseEnergy));
    }
}

TEST_CASE("acquisition pages require proven latch readiness")
{
    auto learn = LatchedSnapshot(TuningSessionPhase::AutomaticLearnSignal);
    CHECK(PresentGui(learn).page == GuiPage::AutomaticLearnSignal);

    learn.tuningSession.evidence.acquisitionLatchReady = false;
    CHECK(PresentGui(learn).page == GuiPage::Preparing);

    learn.tuningSession.evidence.moduleCouldNotBeArmed = true;
    learn.tuningSession.evidence.restorationInProgress = true;
    auto view = PresentGui(learn);
    CHECK(view.page == GuiPage::Restoring);
    CHECK(view.page != GuiPage::AutomaticLearnSignal);
    CHECK(view.tuningOutcome == GuiTuningOutcome::ModuleCouldNotBeArmed);

    learn.tuningSession.phase = TuningSessionPhase::AutomaticEnergy;
    view = PresentGui(learn);
    CHECK(view.page == GuiPage::Restoring);
    CHECK(view.tuningOutcome == GuiTuningOutcome::ModuleCouldNotBeArmed);

    learn.tuningSession.evidence.restorationInProgress = false;
    view = PresentGui(learn);
    CHECK(view.page == GuiPage::Preparing);
    CHECK_FALSE(Allows(
        view.allowedActions,
        GuiAction::CompareChannels));

    learn.tuningSession.evidence.moduleCouldNotBeArmed = false;
    learn.tuningSession.evidence.insufficientData = true;
    view = PresentGui(learn);
    CHECK(view.tuningOutcome == GuiTuningOutcome::InsufficientData);
    CHECK(view.tuningOutcome != GuiTuningOutcome::ModuleCouldNotBeArmed);

    auto energy = LatchedSnapshot(TuningSessionPhase::AutomaticEnergy);
    energy.tuningSession.evidence.learnSignalComplete = true;
    energy.tuningSession.evidence.currentAppliedValuesVerified = true;
    energy.tuningSession.evidence.acquisitionLatchReady = false;
    CHECK(PresentGui(energy).page == GuiPage::Preparing);

    energy.tuningSession.evidence.acquisitionLatchReady = true;
    energy.tuningSession.evidence.currentAppliedValuesVerified = false;
    const auto pendingCandidate = PresentGui(energy);
    CHECK(pendingCandidate.page == GuiPage::AutomaticEnergy);
    CHECK_FALSE(pendingCandidate.claims.currentAppliedValuesVerified);

    auto manual = LatchedSnapshot(TuningSessionPhase::Manual);
    manual.tuningSession.evidence.manualControlsAvailable = false;
    CHECK(PresentGui(manual).page == GuiPage::ManualEnergy);
    CHECK_FALSE(Allows(
        PresentGui(manual).allowedActions,
        GuiAction::CommitManualEdit));
}

TEST_CASE("next group and finished pages require restoration evidence")
{
    using Member = bool TuningSessionEvidence::*;

    auto next = PreparedSnapshot(TuningSessionPhase::NextGroup);
    next.tuningSession.evidence.groupCapturedStateRestored = true;
    next.tuningSession.evidence.groupRestorationReadbackVerified = true;
    next.tuningSession.evidence.recommendationRetained = true;
    CHECK(PresentGui(next).page == GuiPage::NextGroup);

    constexpr std::array<Member, 3> nextFacts = {{
        &TuningSessionEvidence::groupCapturedStateRestored,
        &TuningSessionEvidence::groupRestorationReadbackVerified,
        &TuningSessionEvidence::recommendationRetained,
    }};
    for (const auto member: nextFacts)
    {
        auto missing = next;
        missing.tuningSession.evidence.*member = false;
        const auto view = PresentGui(missing);
        CHECK(view.page == GuiPage::Preparing);
        CHECK_FALSE(view.claims.restorationInProgress);
        CHECK_FALSE(view.claims.recommendationRetained);
    }

    auto nextWithoutControl = next;
    nextWithoutControl.tuningSession.evidence.controlHeld = false;
    CHECK(PresentGui(nextWithoutControl).page == GuiPage::Preparing);

    auto nextWithoutFingerprint = next;
    nextWithoutFingerprint.tuningSession.evidence
        .ownershipFingerprintVerified = false;
    CHECK(PresentGui(nextWithoutFingerprint).page == GuiPage::Preparing);

    auto finished = ReadySnapshot();
    finished.tuningSession.phase = TuningSessionPhase::Finished;
    finished.tuningSession.evidence.sessionCapturedStateRestored = true;
    finished.tuningSession.evidence.sessionRestorationReadbackVerified = true;
    finished.tuningSession.evidence.controllerReleased = true;
    finished.tuningSession.evidence.outputCreated = true;
    CHECK(PresentGui(finished).page == GuiPage::Finished);

    constexpr std::array<Member, 3> finishFacts = {{
        &TuningSessionEvidence::sessionCapturedStateRestored,
        &TuningSessionEvidence::sessionRestorationReadbackVerified,
        &TuningSessionEvidence::controllerReleased,
    }};
    for (const auto member: finishFacts)
    {
        auto missing = finished;
        missing.tuningSession.evidence.*member = false;
        const auto view = PresentGui(missing);
        CHECK(view.page == GuiPage::Preparing);
        CHECK_FALSE(view.claims.restorationInProgress);
        CHECK_FALSE(Allows(view.allowedActions, GuiAction::ReturnHome));
    }

    finished.tuningSession.evidence.outputCreated = false;
    CHECK(PresentGui(finished).page == GuiPage::Preparing);
    finished.tuningSession.evidence.outputDeliberatelyDeclined = true;
    CHECK(PresentGui(finished).page == GuiPage::Finished);

    auto releaseAlone = TunerSnapshot{};
    releaseAlone.tuningSession.evidence.controllerReleased = true;
    CHECK_FALSE(PresentGui(releaseAlone).claims.controllerReleased);
}

TEST_CASE("recovered requires restore readback journal removal and release")
{
    using Member = bool TuningSessionEvidence::*;

    auto recovered = ReadySnapshot();
    auto& evidence = recovered.tuningSession.evidence;
    evidence.recoveryContextEstablished = true;
    evidence.recoveryControllerAndTargetIdentitiesVerified = true;
    evidence.recoveryStoppedStateVerified = true;
    evidence.recoveryComparison = TuningRecoveryComparison::LiveApplied;
    evidence.temporaryValuesRestored = true;
    evidence.recoveryReadbackVerified = true;
    evidence.recoveryJournalCleared = true;
    evidence.recoveryControllerReleased = true;

    auto view = PresentGui(recovered);
    CHECK(view.page == GuiPage::Recovered);
    CHECK(view.claims.recoveryValuesRestoredOrOriginal);
    CHECK(view.claims.recoveryJournalCleared);
    CHECK(view.claims.recoveryControllerReleased);
    CHECK(Allows(view.allowedActions, GuiAction::ContinueToHome));

    auto staleNavigation = recovered;
    staleNavigation.tuningSession.evidence.primaryNavigationAvailable = true;
    staleNavigation.tuningSession.evidence.navigationAwayVerifiedSafe = true;
    const auto recoveredWithStaleNavigation = PresentGui(staleNavigation);
    CHECK_FALSE(Allows(
        recoveredWithStaleNavigation.allowedActions,
        GuiAction::NavigateHome));
    CHECK_FALSE(Allows(
        recoveredWithStaleNavigation.allowedActions,
        GuiAction::NavigateTune));

    constexpr std::array<Member, 7> requiredFacts = {{
        &TuningSessionEvidence::recoveryControllerAndTargetIdentitiesVerified,
        &TuningSessionEvidence::recoveryStoppedStateVerified,
        &TuningSessionEvidence::temporaryValuesRestored,
        &TuningSessionEvidence::recoveryReadbackVerified,
        &TuningSessionEvidence::recoveryJournalCleared,
        &TuningSessionEvidence::recoveryControllerReleased,
        &TuningSessionEvidence::noRecoveryPending,
    }};
    for (const auto member: requiredFacts)
    {
        auto missing = recovered;
        missing.tuningSession.evidence.*member = false;
        const auto missingView = PresentGui(missing);
        CHECK(missingView.page == GuiPage::Recovery);
        CHECK_FALSE(Allows(
            missingView.allowedActions,
            GuiAction::ContinueToHome));
    }

    auto unclassified = recovered;
    unclassified.tuningSession.evidence.recoveryComparison =
        TuningRecoveryComparison::NotCompared;
    CHECK(PresentGui(unclassified).page == GuiPage::Recovery);

    recovered.tuningSession.evidence.temporaryValuesRestored = false;
    recovered.tuningSession.evidence.valuesAlreadyOriginal = true;
    recovered.tuningSession.evidence.recoveryComparison =
        TuningRecoveryComparison::LiveOriginal;
    CHECK(PresentGui(recovered).page == GuiPage::Recovered);

    auto noRecoveryContext = recovered;
    noRecoveryContext.tuningSession.evidence.recoveryContextEstablished =
        false;
    CHECK(PresentGui(noRecoveryContext).page == GuiPage::HomeReady);
    CHECK_FALSE(Allows(
        PresentGui(noRecoveryContext).allowedActions,
        GuiAction::ContinueToHome));

    auto stillRunning = recovered;
    stillRunning.tuningSession.evidence.recoveryInProgress = true;
    CHECK(PresentGui(stillRunning).page == GuiPage::Recovery);
    CHECK_FALSE(PresentGui(stillRunning).claims.recoveryControllerReleased);

    auto outstandingJournal = recovered;
    outstandingJournal.recoveryRecordAvailable = true;
    outstandingJournal.recoveryJournalStatus =
        RecoveryJournalStatus::Pending;
    outstandingJournal.tuningSession.evidence.recoveryRecordPresent = true;
    outstandingJournal.tuningSession.evidence.noRecoveryPending = false;
    CHECK(PresentGui(outstandingJournal).page == GuiPage::Recovery);
    CHECK_FALSE(
        PresentGui(outstandingJournal).claims.recoveryJournalCleared);
    CHECK_FALSE(Allows(
        PresentGui(outstandingJournal).allowedActions,
        GuiAction::ContinueToHome));

    auto readbackOnly = RecoverySnapshot();
    readbackOnly.tuningSession.evidence.recoveryReadbackVerified = true;
    readbackOnly.tuningSession.evidence.recoveryJournalCleared = true;
    readbackOnly.tuningSession.evidence.recoveryControllerReleased = true;
    CHECK_FALSE(
        PresentGui(readbackOnly).claims.recoveryValuesRestoredOrOriginal);
    CHECK_FALSE(PresentGui(readbackOnly).claims.recoveryJournalCleared);
    CHECK_FALSE(PresentGui(readbackOnly).claims.recoveryControllerReleased);
}

TEST_CASE("safety pages and actions require their complete evidence bundles")
{
    auto conflict = ReadySnapshot();
    auto& conflictEvidence = conflict.tuningSession.evidence;
    conflictEvidence.activeControllerUseDetected = true;
    conflictEvidence.noControlTaken = true;
    conflictEvidence.noStateChangingCommandsSent = true;
    conflictEvidence.primaryNavigationAvailable = true;
    conflictEvidence.navigationAwayVerifiedSafe = true;
    auto view = PresentGui(conflict);
    CHECK(view.page == GuiPage::ControllerConflict);
    CHECK(view.claims.noControlTaken);
    CHECK(view.claims.noStateChangingCommandsSent);
    CHECK_FALSE(Allows(view.allowedActions, GuiAction::NavigateHome));
    CHECK_FALSE(Allows(view.allowedActions, GuiAction::NavigateTune));

    conflictEvidence.noStateChangingCommandsSent = false;
    view = PresentGui(conflict);
    CHECK(view.page == GuiPage::ControllerConflict);
    CHECK(view.claims.noControlTaken);
    CHECK_FALSE(view.claims.noStateChangingCommandsSent);
    CHECK_FALSE(Allows(view.allowedActions, GuiAction::CheckAgain));

    auto uncertain = PreparedSnapshot(TuningSessionPhase::Manual);
    auto& uncertainEvidence = uncertain.tuningSession.evidence;
    uncertainEvidence.ownershipUncertain = true;
    uncertainEvidence.controlsFrozen = true;
    uncertainEvidence.noBlindRollbackOrCleanupSent = true;
    uncertainEvidence.reconnectAvailable = true;
    uncertainEvidence.primaryNavigationAvailable = true;
    uncertainEvidence.navigationAwayVerifiedSafe = true;
    view = PresentGui(uncertain);
    CHECK(view.page == GuiPage::OwnershipUncertain);
    CHECK(view.claims.controlsFrozen);
    CHECK(view.claims.noBlindRollbackOrCleanupSent);
    CHECK(Allows(view.allowedActions, GuiAction::ReconnectAndVerify));
    CHECK_FALSE(Allows(view.allowedActions, GuiAction::NavigateHome));
    CHECK_FALSE(Allows(view.allowedActions, GuiAction::NavigateTune));

    uncertainEvidence.recoveryRecordDurable = false;
    CHECK_FALSE(Allows(
        PresentGui(uncertain).allowedActions,
        GuiAction::ReconnectAndVerify));

    auto blocked = RecoverySnapshot();
    auto& blockedEvidence = blocked.tuningSession.evidence;
    blockedEvidence.recoveryComparison =
        TuningRecoveryComparison::InsufficientEvidence;
    blockedEvidence.unexpectedRecoveryValueNotOverwritten = true;
    blockedEvidence.recoveryRecordRetained = true;
    blockedEvidence.noRecoveryWritesSent = true;
    blockedEvidence.guardedRecoveryRetryAvailable = true;
    blockedEvidence.primaryNavigationAvailable = true;
    blockedEvidence.navigationAwayVerifiedSafe = true;
    view = PresentGui(blocked);
    CHECK(view.page == GuiPage::RecoveryBlocked);
    CHECK_FALSE(view.claims.unexpectedRecoveryValueNotOverwritten);
    CHECK(view.claims.recoveryRecordRetained);
    CHECK(view.claims.noRecoveryWritesSent);
    CHECK(Allows(view.allowedActions, GuiAction::RetryVerification));
    CHECK_FALSE(Allows(view.allowedActions, GuiAction::NavigateHome));
    CHECK_FALSE(Allows(view.allowedActions, GuiAction::NavigateTune));

    blockedEvidence.noRecoveryWritesSent = false;
    view = PresentGui(blocked);
    CHECK(view.page == GuiPage::RecoveryBlocked);
    CHECK_FALSE(view.claims.noRecoveryWritesSent);
    CHECK(Allows(view.allowedActions, GuiAction::RetryVerification));

    auto recovery = RecoverySnapshot();
    recovery.tuningSession.evidence.primaryNavigationAvailable = true;
    recovery.tuningSession.evidence.navigationAwayVerifiedSafe = true;
    view = PresentGui(recovery);
    CHECK(view.page == GuiPage::Recovery);
    CHECK_FALSE(Allows(view.allowedActions, GuiAction::NavigateHome));
    CHECK_FALSE(Allows(view.allowedActions, GuiAction::NavigateTune));

    blockedEvidence.recoveryRecordRetained = false;
    view = PresentGui(blocked);
    CHECK(view.page == GuiPage::RecoveryBlocked);
    CHECK_FALSE(view.claims.recoveryRecordRetained);
    CHECK_FALSE(Allows(view.allowedActions, GuiAction::RetryVerification));

    auto malformed = ReadySnapshot();
    malformed.recoveryJournalStatus = RecoveryJournalStatus::Malformed;
    malformed.tuningSession.evidence.recoveryRecordRetained = true;
    malformed.tuningSession.evidence.noRecoveryWritesSent = true;
    malformed.tuningSession.evidence.guardedRecoveryRetryAvailable = true;
    view = PresentGui(malformed);
    CHECK(view.page == GuiPage::RecoveryBlocked);
    CHECK(view.headerConnection
          == GuiHeaderConnectionStatus::RecoveryBlocked);
    CHECK(Allows(view.allowedActions, GuiAction::RetryVerification));
}

TEST_CASE("write and restoration intents require current ownership evidence")
{
    using Member = bool TuningSessionEvidence::*;

    auto manual = LatchedSnapshot(TuningSessionPhase::Manual);
    auto& evidence = manual.tuningSession.evidence;
    evidence.manualControlsAvailable = true;
    evidence.manualEditValid = true;
    evidence.currentAppliedValuesVerified = true;
    CHECK(Allows(
        PresentGui(manual).allowedActions,
        GuiAction::CommitManualEdit));
    CHECK(Allows(
        PresentGui(manual).allowedActions,
        GuiAction::StopAndRestore));

    constexpr std::array<Member, 4> writeFacts = {{
        &TuningSessionEvidence::controlHeld,
        &TuningSessionEvidence::ownershipFingerprintVerified,
        &TuningSessionEvidence::operationIdle,
        &TuningSessionEvidence::recoveryRecordDurable,
    }};

    for (const auto member: writeFacts)
    {
        auto missing = manual;
        missing.tuningSession.evidence.*member = false;
        const auto missingView = PresentGui(missing);
        CHECK_FALSE(Allows(
            missingView.allowedActions,
            GuiAction::CommitManualEdit));
    }

    constexpr std::array<Member, 6> restoreFacts = {{
        &TuningSessionEvidence::controlHeld,
        &TuningSessionEvidence::ownershipFingerprintVerified,
        &TuningSessionEvidence::liveRestoreSnapshotCaptured,
        &TuningSessionEvidence::recoveryRecordDurable,
        &TuningSessionEvidence::restorationRequired,
        &TuningSessionEvidence::cancelAndRestoreAvailable,
    }};
    for (const auto member: restoreFacts)
    {
        auto missing = manual;
        missing.tuningSession.evidence.*member = false;
        CHECK_FALSE(Allows(
            PresentGui(missing).allowedActions,
            GuiAction::StopAndRestore));
    }

    auto busy = manual;
    busy.tuningSession.evidence.operationIdle = false;
    CHECK_FALSE(Allows(
        PresentGui(busy).allowedActions,
        GuiAction::CommitManualEdit));
    CHECK(Allows(
        PresentGui(busy).allowedActions,
        GuiAction::StopAndRestore));

    manual = LatchedSnapshot(TuningSessionPhase::Manual);
    manual.tuningSession.evidence.manualControlsAvailable = true;
    manual.tuningSession.evidence.manualEditValid = true;
    manual.tuningSession.evidence.currentAppliedValuesVerified = true;
    manual.tuningSession.evidence.controllerReleased = true;
    const auto view = PresentGui(manual);
    CHECK_FALSE(view.claims.controlHeldAndVerified);
    CHECK_FALSE(Allows(
        view.allowedActions,
        GuiAction::CommitManualEdit));
}

TEST_CASE("connection checks and both tuning paths have explicit gates")
{
    TunerSnapshot connected;
    auto& connection = connected.tuningSession.evidence;
    connection.currentConnectionRequestValid = true;
    connection.operationIdle = true;
    auto view = PresentGui(connected);
    CHECK(view.page == GuiPage::HomeDisconnected);
    CHECK(Allows(view.allowedActions, GuiAction::Check));

    connection.controllerConnected = true;
    CHECK(Allows(
        PresentGui(connected).allowedActions,
        GuiAction::Check));

    connection.currentConnectionRequestValid = false;
    CHECK_FALSE(Allows(
        PresentGui(connected).allowedActions,
        GuiAction::Check));

    auto group = PreparedSnapshot(TuningSessionPhase::Group);
    auto& evidence = group.tuningSession.evidence;
    evidence.goalSelected = true;
    evidence.groupSelectionValid = true;
    evidence.channelSelectionValid = true;
    evidence.manualPathSelected = true;
    CHECK(Allows(
        PresentGui(group).allowedActions,
        GuiAction::BeginManual));

    evidence.channelSelectionValid = false;
    CHECK_FALSE(Allows(
        PresentGui(group).allowedActions,
        GuiAction::BeginManual));
}

TEST_CASE("comparison preserves its underlying tuning context")
{
    auto manual = LatchedSnapshot(TuningSessionPhase::Manual);
    auto& manualEvidence = manual.tuningSession.evidence;
    manualEvidence.manualControlsAvailable = true;
    manualEvidence.currentAppliedValuesVerified = true;
    manualEvidence.undoAvailable = true;
    manualEvidence.comparisonAvailable = true;

    const GuiPresentationSelection compareManual{
        GuiDrawer::None, false, GuiManualPanel::Timing, true};
    auto view = PresentGui(manual, compareManual);
    CHECK(view.page == GuiPage::CompareChannels);
    CHECK(Allows(view.allowedActions, GuiAction::SelectTimingControls));
    CHECK(Allows(view.allowedActions, GuiAction::UndoManualChange));
    CHECK(Allows(view.allowedActions, GuiAction::FinishManual));
    CHECK(Allows(view.allowedActions, GuiAction::ReturnFromComparison));

    auto automatic = LatchedSnapshot(TuningSessionPhase::AutomaticEnergy);
    automatic.tuningSession.evidence.learnSignalComplete = true;
    automatic.tuningSession.evidence.comparisonAvailable = true;
    view = PresentGui(automatic, compareManual);
    CHECK(view.page == GuiPage::CompareChannels);
    CHECK_FALSE(Allows(
        view.allowedActions,
        GuiAction::SelectTimingControls));
    CHECK_FALSE(Allows(
        view.allowedActions,
        GuiAction::UndoManualChange));
    CHECK(Allows(
        view.allowedActions,
        GuiAction::ReturnFromComparison));
}

TEST_CASE("a completed result always retains an evidence-backed exit")
{
    auto result = LatchedSnapshot(TuningSessionPhase::GroupResult);
    auto& evidence = result.tuningSession.evidence;
    evidence.groupResultComplete = true;
    evidence.resultExitAvailable = true;

    auto view = PresentGui(result);
    CHECK(view.page == GuiPage::GroupResult);
    CHECK_FALSE(evidence.recommendationVerified);
    CHECK(Allows(view.allowedActions, GuiAction::TuneAnotherGroup));
    CHECK(Allows(view.allowedActions, GuiAction::SaveAndFinish));

    evidence.resultExitAvailable = false;
    view = PresentGui(result);
    CHECK_FALSE(Allows(view.allowedActions, GuiAction::TuneAnotherGroup));
    CHECK(Allows(view.allowedActions, GuiAction::StopAndRestore));

    evidence.resultExitAvailable = true;
    evidence.restorationRequired = false;
    evidence.groupCapturedStateRestored = true;
    evidence.groupRestorationReadbackVerified = true;
    view = PresentGui(result);
    CHECK(Allows(view.allowedActions, GuiAction::TuneAnotherGroup));
    CHECK(Allows(view.allowedActions, GuiAction::SaveAndFinish));
    CHECK_FALSE(Allows(view.allowedActions, GuiAction::StopAndRestore));

    result.tuningSession.phase = TuningSessionPhase::NextGroup;
    evidence.noRecommendationAvailableVerified = true;
    CHECK(PresentGui(result).page == GuiPage::NextGroup);
}

TEST_CASE("drawer and navigation actions remain evidence-gated")
{
    auto snapshot = ReadySnapshot();
    auto view = PresentGui(snapshot);
    CHECK_FALSE(Allows(view.allowedActions, GuiAction::OpenDetails));
    CHECK_FALSE(Allows(view.allowedActions, GuiAction::OpenLogs));
    CHECK_FALSE(Allows(view.allowedActions, GuiAction::OpenHelp));
    CHECK_FALSE(Allows(view.allowedActions, GuiAction::NavigateHome));
    CHECK_FALSE(Allows(view.allowedActions, GuiAction::NavigateTune));

    auto& evidence = snapshot.tuningSession.evidence;
    evidence.detailsAvailable = true;
    evidence.logsAvailable = true;
    evidence.helpAvailable = true;
    evidence.primaryNavigationAvailable = true;
    view = PresentGui(snapshot);
    CHECK(Allows(view.allowedActions, GuiAction::OpenDetails));
    CHECK(Allows(view.allowedActions, GuiAction::OpenLogs));
    CHECK(Allows(view.allowedActions, GuiAction::OpenHelp));
    CHECK(Allows(view.allowedActions, GuiAction::NavigateHome));
    CHECK(Allows(view.allowedActions, GuiAction::NavigateTune));

    auto controlled = PreparedSnapshot(TuningSessionPhase::AutomaticEnergy);
    controlled.tuningSession.evidence.acquisitionLatchReady = true;
    controlled.tuningSession.evidence.learnSignalComplete = true;
    controlled.tuningSession.evidence.primaryNavigationAvailable = true;
    controlled.tuningSession.evidence.navigationAwayVerifiedSafe = true;
    controlled.tuningSession.evidence.operationIdle = false;
    view = PresentGui(controlled);
    CHECK(Allows(view.allowedActions, GuiAction::NavigateHome));
    CHECK(Allows(view.allowedActions, GuiAction::StopAndRestore));
    CHECK_FALSE(Allows(view.allowedActions, GuiAction::NavigateTune));
}

TEST_CASE("SSH expansion remains presentation-only")
{
    auto snapshot = ReadySnapshot();
    snapshot.tuningSession.evidence.endpointEditingAllowed = true;

    const auto expanded = PresentGui(
        snapshot,
        GuiPresentationSelection{GuiDrawer::None, true});
    CHECK(expanded.page == GuiPage::HomeSshExpanded);
    CHECK(expanded.headerConnection == GuiHeaderConnectionStatus::Connected);
    CHECK(Allows(expanded.allowedActions, GuiAction::StartTuning));
    CHECK(Allows(expanded.allowedActions, GuiAction::CollapseSshSettings));
    CHECK(snapshot.tuningSession.phase == TuningSessionPhase::Home);
}
