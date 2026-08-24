#ifndef FIDGET_CORE_TUNING_SESSION_STATE_H
#define FIDGET_CORE_TUNING_SESSION_STATE_H

namespace fidget {

// Backend workflow intent for the redesigned tuner presentation. These values
// describe session progress only. The presenter requires separate positive
// evidence before it exposes claims or actions for any phase.
enum class TuningSessionPhase
{
    Home,
    Preparing,
    Goal,
    Group,
    AutomaticLearnSignal,
    AutomaticEnergy,
    AutomaticTiming,
    Manual,
    GroupResult,
    Restoring,
    NextGroup,
    Finished,
};

enum class TuningRecoveryComparison
{
    NotCompared,
    LiveOriginal,
    LiveApplied,
    VerifiedMixed,
    Unexpected,
    InsufficientEvidence,
};

// Positive backend facts consumed by the GUI presenter. A false value means
// the corresponding fact has not been established. In particular, absence of
// control is not evidence of release, and a requested phase is not evidence
// that its hardware work completed.
struct TuningSessionEvidence
{
    bool endpointInputsValid = false;
    bool targetModuleAddressValid = false;
    bool endpointEditingAllowed = false;
    bool currentConnectionRequestValid = false;
    bool controllerConnected = false;
    bool controllerIdentityVerified = false;
    bool controllerVerificationFresh = false;
    bool targetIdentityAndFirmwareVerified = false;
    bool controllerIdleVerified = false;
    bool targetAcquisitionStoppedVerified = false;
    bool targetVerificationFresh = false;
    bool connectionVerificationFresh = false;
    bool noRecoveryPending = false;
    bool operationIdle = false;

    bool controlHeld = false;
    bool ownershipFingerprintVerified = false;
    bool liveRestoreSnapshotCaptured = false;
    bool recoveryRecordDurable = false;
    bool workspaceStartingSettingsResolved = false;
    bool acquisitionPrepared = false;
    bool acquisitionLatchReady = false;
    bool moduleCouldNotBeArmed = false;
    bool insufficientData = false;

    bool goalSelected = false;
    bool automaticPathSelected = false;
    bool manualPathSelected = false;
    bool groupSelectionValid = false;
    bool channelSelectionValid = false;
    bool learnSignalComplete = false;
    bool currentAppliedValuesVerified = false;
    bool comparisonAvailable = false;
    bool manualControlsAvailable = false;
    bool manualEditValid = false;
    bool undoAvailable = false;
    bool redoAvailable = false;
    bool workingDiffersFromOriginal = false;
    bool groupResultComplete = false;
    bool recommendationVerified = false;
    bool settingChangesVerified = false;

    bool restorationRequired = false;
    bool restorationInProgress = false;
    bool cancelAndRestoreAvailable = false;
    bool groupCapturedStateRestored = false;
    bool groupRestorationReadbackVerified = false;
    bool recommendationRetained = false;
    bool noRecommendationAvailableVerified = false;
    bool resultExitAvailable = false;
    bool sessionCapturedStateRestored = false;
    bool sessionRestorationReadbackVerified = false;
    bool controllerReleased = false;
    bool outputCreated = false;
    bool outputDeliberatelyDeclined = false;

    bool activeControllerUseDetected = false;
    bool noControlTaken = false;
    bool noVmeOrModuleSettingWritesSent = false;
    bool ownershipUncertain = false;
    bool controlsFrozen = false;
    bool noBlindRollbackOrCleanupSent = false;
    bool reconnectAvailable = false;

    bool recoveryRecordPresent = false;
    bool recoveryContextEstablished = false;
    bool recoveryInProgress = false;
    bool recoveryControllerAndTargetIdentitiesVerified = false;
    bool recoveryStoppedStateVerified = false;
    TuningRecoveryComparison recoveryComparison =
        TuningRecoveryComparison::NotCompared;
    bool unexpectedRecoveryValueNotOverwritten = false;
    bool recoveryRecordRetained = false;
    bool noRecoveryWritesSent = false;
    bool temporaryValuesRestored = false;
    bool valuesAlreadyOriginal = false;
    bool recoveryReadbackVerified = false;
    bool recoveryJournalCleared = false;
    bool recoveryControllerReleased = false;
    bool guardedRecoveryRetryAvailable = false;

    bool detailsAvailable = false;
    bool logsAvailable = false;
    bool helpAvailable = false;
    bool primaryNavigationAvailable = false;
    bool navigationAwayVerifiedSafe = false;
};

struct TuningSessionState
{
    TuningSessionPhase phase = TuningSessionPhase::Home;
    TuningSessionEvidence evidence;
};

} // namespace fidget

#endif
