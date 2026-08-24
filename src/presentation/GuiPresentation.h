#ifndef FIDGET_PRESENTATION_GUI_PRESENTATION_H
#define FIDGET_PRESENTATION_GUI_PRESENTATION_H

#include "core/TunerSnapshot.h"

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

namespace fidget {

enum class GuiPage
{
    HomeDisconnected,
    HomeReady,
    Preparing,
    Goal,
    Group,
    AutomaticLearnSignal,
    AutomaticEnergy,
    AutomaticTiming,
    ManualEnergy,
    ManualTiming,
    CompareChannels,
    GroupResult,
    Restoring,
    NextGroup,
    Finished,
    ControllerConflict,
    OwnershipUncertain,
    Recovery,
    RecoveryBlocked,
    Recovered,
    HomeSshExpanded,
    Count,
};

inline constexpr std::size_t GuiPageCount =
    static_cast<std::size_t>(GuiPage::Count);

enum class GuiDrawer
{
    None,
    Details,
    Logs,
    Help,
    SettingChanges,
    Count,
};

inline constexpr std::size_t GuiDrawerCount =
    static_cast<std::size_t>(GuiDrawer::Count);

enum class GuiManualPanel
{
    Energy,
    Timing,
};

enum class GuiHeaderConnectionStatus
{
    Unknown,
    NotConnected,
    Connected,
    HasControl,
    ControllerConflict,
    OwnershipUncertain,
    RecoveryInProgress,
    RecoveryBlocked,
    Count,
};

enum class GuiTuningOutcome
{
    None,
    ModuleCouldNotBeArmed,
    InsufficientData,
};

enum class GuiConflictRetry
{
    None,
    Connect,
    Check,
};

enum class GuiTargetAddressDisplay
{
    HomeMvmeShorthand,
    DetailsExpandedA32,
};

enum class GuiAction
{
    NavigateHome,
    NavigateTune,
    Connect,
    Check,
    StartTuning,
    ChooseEnergy,
    ChooseTiming,
    ChooseBoth,
    ChooseManual,
    BeginAutomatic,
    BeginManual,
    StopAndRestore,
    SelectEnergyControls,
    SelectTimingControls,
    CommitManualEdit,
    UndoManualChange,
    RedoManualChange,
    ReturnToOriginal,
    FinishManual,
    CompareChannels,
    ReturnFromComparison,
    ReviewSettingChanges,
    TuneAnotherGroup,
    SaveAndFinish,
    ReturnHome,
    CheckAgain,
    ReconnectAndVerify,
    RetryVerification,
    ContinueToHome,
    OpenDetails,
    OpenLogs,
    OpenHelp,
    CloseDrawer,
    ExpandSshSettings,
    CollapseSshSettings,
    Count,
};

inline constexpr std::size_t GuiActionCount =
    static_cast<std::size_t>(GuiAction::Count);

using GuiActionSet = std::bitset<GuiActionCount>;

[[nodiscard]] bool Allows(
    const GuiActionSet& actions,
    GuiAction action) noexcept;

struct GuiPresentationSelection
{
    GuiDrawer drawer = GuiDrawer::None;
    bool sshSettingsExpanded = false;
    GuiManualPanel manualPanel = GuiManualPanel::Energy;
    bool compareChannels = false;
};

// Claims are deliberately derived from independent evidence. Widgets can use
// these values without treating a page or backend phase as proof that hardware
// is verified, restored, or released.
struct GuiEvidenceClaims
{
    bool controllerEndpointVerified = false;
    bool targetModuleVerified = false;
    bool controllerReady = false;
    bool controlHeldAndVerified = false;
    bool controllerAndTargetIdentitiesVerified = false;
    bool liveRestoreSnapshotCaptured = false;
    bool recoveryRecordDurable = false;
    bool workspaceStartingSettingsResolved = false;
    bool acquisitionPrepared = false;
    bool acquisitionLatchReady = false;
    bool currentAppliedValuesVerified = false;
    bool recommendationVerified = false;
    bool settingChangesVerified = false;
    bool restorationInProgress = false;

    bool groupCapturedStateRestored = false;
    bool groupRestorationReadbackVerified = false;
    bool recommendationRetained = false;
    bool sessionCapturedStateRestored = false;
    bool sessionRestorationReadbackVerified = false;
    bool controllerReleased = false;
    bool outputCreatedOrDeclined = false;

    bool noControlTaken = false;
    bool noVmeOrModuleSettingWritesSent = false;
    bool controlsFrozen = false;
    bool noBlindRollbackOrCleanupSent = false;

    bool recoveryIdentitiesVerified = false;
    bool recoveryStoppedStateVerified = false;
    bool unexpectedRecoveryValueNotOverwritten = false;
    bool recoveryRecordRetained = false;
    bool noRecoveryWritesSent = false;
    bool recoveryValuesRestoredOrOriginal = false;
    bool recoveryReadbackVerified = false;
    bool recoveryJournalCleared = false;
    bool recoveryControllerReleased = false;
};

struct GuiViewState
{
    GuiPage page = GuiPage::HomeDisconnected;
    GuiDrawer drawer = GuiDrawer::None;
    GuiHeaderConnectionStatus headerConnection =
        GuiHeaderConnectionStatus::Unknown;
    GuiTuningOutcome tuningOutcome = GuiTuningOutcome::None;
    GuiConflictRetry conflictRetry = GuiConflictRetry::None;
    GuiActionSet allowedActions;
    GuiEvidenceClaims claims;
    // This is a display projection of TunerTargetSelection's one canonical
    // parsed address. No second address is parsed or stored as authority.
    std::optional<std::uint32_t> targetModuleAddressA32;
};

[[nodiscard]] GuiViewState PresentGui(
    const TunerSnapshot& snapshot,
    const GuiPresentationSelection& selection = {}) noexcept;

[[nodiscard]] const char* GuiHeaderConnectionStatusText(
    GuiHeaderConnectionStatus status) noexcept;

[[nodiscard]] std::string GuiTargetAddressText(
    std::uint32_t fullA32Address,
    GuiTargetAddressDisplay display);

} // namespace fidget

#endif
