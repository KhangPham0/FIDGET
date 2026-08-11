#include "hardware/OwnershipService.h"

#include "core/CrateProject.h"
#include "core/ScpProfile.h"
#include "core/ScpRegistry.h"
#include "core/ScpTransactionPlan.h"
#include "core/StartupAudit.h"
#include "core/StartupPreparation.h"
#include "core/VmeProtocol.h"
#include "hardware/DeterministicStartupOperation.h"
#include "hardware/VmeTransaction.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <system_error>
#include <utility>

namespace fidget {
namespace {

constexpr int ReadOnlyTransactionAttempts = 3;
constexpr int MaximumReadOnlyResponseDatagrams = 8;
constexpr int CommandReceiveTimeoutMilliseconds = 500;
constexpr std::size_t CommandResponseBufferSize = 9000U;

const char* InUseMessage =
    "MVLC DAQ mode is active. Stop the MVME run and disconnect its VME "
    "controller before opening the tuner.";
const char* InvalidHardwareMessage =
    "The target answered, but register 0x6008 did not identify an MVLC "
    "(expected 0x5008).";
const char* IdleMessage =
    "Status check passed. MVLC DAQ mode is idle; the check socket has been "
    "closed.";
const char* SessionOpenMessage =
    "Tuner preflight passed. No active MVLC DAQ mode was detected.";
const char* DisconnectedMessage = "No tuner session";

} // namespace

OwnershipService::OwnershipService(
    std::unique_ptr<ICommandTransport> transport,
    std::chrono::milliseconds watchdogInterval)
    : transport_(std::move(transport))
    , snapshot_(std::make_shared<const TunerSnapshot>())
    , watchdogInterval_(watchdogInterval)
{
    if (!transport_)
    {
        TunerSnapshot snapshot;
        snapshot.ownership = GuidedTunerOwnershipState::Failed;
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Error,
            "The ownership service has no command transport.");
    }
}

OwnershipService::~OwnershipService()
{
    serviceStopRequested_.store(true);
    StopWatchdog();

    if (transport_ && worker_.IsAcceptingTasks())
    {
        auto close = worker_.Post([this] { transport_->Close(); });
        try
        {
            close.get();
        }
        catch (const std::exception&)
        {
        }
    }
    worker_.StopAndJoin();
}

std::shared_ptr<const TunerSnapshot>
OwnershipService::CurrentSnapshot() const
{
    return std::atomic_load_explicit(
        &snapshot_, std::memory_order_acquire);
}

void OwnershipService::Submit(TunerCommand command)
{
    if (std::holds_alternative<UseCrateProjectCommand>(command))
    {
        StopWatchdog();
        auto project = std::get<UseCrateProjectCommand>(std::move(command));
        (void)worker_.Post(
            [this, project = std::move(project)]() mutable {
                UseProject(std::move(project));
            });
        return;
    }
    if (std::holds_alternative<ClearCrateProjectCommand>(command))
    {
        StopWatchdog();
        (void)worker_.Post([this] { ClearProject(); });
        return;
    }
    if (const auto* handoff =
            std::get_if<SetMvmeHandoffConfirmedCommand>(&command))
    {
        const bool confirmed = handoff->confirmed;
        (void)worker_.Post(
            [this, confirmed] { SetHandoffConfirmed(confirmed); });
        return;
    }
    if (std::holds_alternative<CheckStatusCommand>(command))
    {
        if (CurrentSnapshot()->ownership
            != GuidedTunerOwnershipState::SessionOpen)
        {
            StopWatchdog();
        }
        (void)worker_.Post([this] { CheckStatus(); });
        return;
    }
    if (std::holds_alternative<OpenSessionCommand>(command))
    {
        if (CurrentSnapshot()->ownership
            == GuidedTunerOwnershipState::Idle)
        {
            StopWatchdog();
        }
        (void)worker_.Post([this] { OpenSession(); });
        return;
    }
    if (std::holds_alternative<RunStartupAuditCommand>(command))
    {
        (void)worker_.Post([this] { RunStartupAudit(); });
        return;
    }
    if (std::holds_alternative<CaptureConfigurationCommand>(command))
    {
        (void)worker_.Post([this] { CaptureConfiguration(); });
        return;
    }
    if (const auto* save = std::get_if<SaveProfileCommand>(&command))
    {
        const std::string path = save->path;
        (void)worker_.Post([this, path] { SaveProfile(path); });
        return;
    }
    if (const auto* load = std::get_if<LoadProfileCommand>(&command))
    {
        const std::string path = load->path;
        (void)worker_.Post([this, path] { LoadProfile(path); });
        return;
    }
    if (const auto* apply = std::get_if<ApplyProfileRowCommand>(&command))
    {
        const auto request = *apply;
        (void)worker_.Post([this, request] { ApplyProfileRow(request); });
        return;
    }
    if (std::holds_alternative<ApplyAllDifferencesCommand>(command))
    {
        (void)worker_.Post([this] { ApplyAllDifferences(); });
        return;
    }
    if (const auto* startup =
            std::get_if<RunDeterministicStartupCommand>(&command))
    {
        const auto request = *startup;
        (void)worker_.Post(
            [this, request] { RunDeterministicStartup(request); });
        return;
    }

    StopWatchdog();
    (void)worker_.Post([this] { ReleaseSession(); });
}

std::future<PreWriteGateResult> OwnershipService::VerifyPreWriteGate(
    std::string operationName)
{
    return worker_.Post(
        [this, operationName = std::move(operationName)] {
            return CheckPreWriteGate(operationName);
        });
}

void OwnershipService::UseProject(UseCrateProjectCommand command)
{
    if (transport_)
    {
        transport_->Close();
    }

    const auto validation = ValidateCrateProject(command.project);
    if (!validation.success)
    {
        TunerSnapshot snapshot;
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Error,
            "The crate project is invalid.",
            validation.message);
        return;
    }
    if (command.activeModuleIndex >= command.project.modules.size())
    {
        TunerSnapshot snapshot;
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Error,
            "The active module index is outside the crate project.");
        return;
    }

    project_ = std::move(command.project);
    activeModuleIndex_ = command.activeModuleIndex;
    const auto& module = project_.modules[activeModuleIndex_];

    TunerSnapshot snapshot;
    snapshot.projectActive = true;
    snapshot.projectPath = std::move(command.projectPath);
    snapshot.mvlcHost = project_.mvlcHost;
    snapshot.mvlcCommandPort = project_.mvlcCommandPort;
    snapshot.activeModuleIndex = activeModuleIndex_;
    snapshot.activeModuleName = module.name;
    snapshot.activeModuleBaseAddress = module.baseAddress;
    snapshot.activeModuleBackend = module.backend;
    snapshot.activeModuleProfilePath = module.profilePath;
    snapshot.targetSupported = MdppBackendImplemented(module.backend);
    snapshot.ownership = GuidedTunerOwnershipState::Disconnected;

    const auto level = snapshot.targetSupported
        ? TunerStatusLevel::Success
        : TunerStatusLevel::Warning;
    const std::string detail = snapshot.targetSupported
        ? validation.message
        : "The selected QDC backend is not implemented.";
    PublishStatus(
        std::move(snapshot), level, "The crate project is active.", detail);
}

void OwnershipService::ClearProject()
{
    if (transport_)
    {
        transport_->Close();
    }
    project_ = {};
    activeModuleIndex_ = 0U;

    TunerSnapshot snapshot;
    PublishStatus(
        std::move(snapshot),
        TunerStatusLevel::Information,
        "No crate project is active.");
}

void OwnershipService::CheckStatus()
{
    auto snapshot = *CurrentSnapshot();
    if (!transport_)
    {
        snapshot.ownership = GuidedTunerOwnershipState::Failed;
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Error,
            "The ownership service has no command transport.");
        return;
    }
    if (!snapshot.projectActive)
    {
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Warning,
            "Select a valid crate project before checking the controller.");
        return;
    }
    if (snapshot.ownership == GuidedTunerOwnershipState::SessionOpen)
    {
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Information,
            "The tuner session is already open.");
        return;
    }

    snapshot.ownership = GuidedTunerOwnershipState::Checking;
    snapshot.controllerReadingsValid = false;
    PublishStatus(
        snapshot,
        TunerStatusLevel::Information,
        "Checking MVLC ownership status.");
    ProbeController(std::move(snapshot), false);
}

void OwnershipService::SetHandoffConfirmed(bool confirmed)
{
    auto snapshot = *CurrentSnapshot();
    snapshot.mvmeHandoffConfirmed = confirmed;
    PublishStatus(
        std::move(snapshot),
        confirmed ? TunerStatusLevel::Success : TunerStatusLevel::Information,
        confirmed
            ? "MVME handoff has been confirmed."
            : "MVME handoff confirmation has been cleared.");
}

void OwnershipService::OpenSession()
{
    auto snapshot = *CurrentSnapshot();
    if (!transport_)
    {
        snapshot.ownership = GuidedTunerOwnershipState::Failed;
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Error,
            "The ownership service has no command transport.");
        return;
    }
    if (snapshot.ownership != GuidedTunerOwnershipState::Idle)
    {
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Warning,
            "A verified idle status check is required before opening the "
            "tuner session.");
        return;
    }
    if (!snapshot.mvmeHandoffConfirmed)
    {
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Warning,
            "An idle DAQ-mode register cannot prove that an idle MVME "
            "process has released USB. Confirmation is still required "
            "before opening a session.");
        return;
    }

    snapshot.ownership = GuidedTunerOwnershipState::Checking;
    snapshot.startupAuditCompleteForTarget = false;
    snapshot.startupAuditReady = false;
    snapshot.startupAudit = {};
    snapshot.configurationCompleteForTarget = false;
    snapshot.configurationFresh = false;
    snapshot.configurationCapture = {};
    snapshot.singleRepairResult = {};
    snapshot.bulkApplyResult = {};
    snapshot.deterministicStartupPassed = false;
    snapshot.deterministicStartupResult = {};
    RefreshProfileComparison(snapshot);
    PublishStatus(
        snapshot,
        TunerStatusLevel::Information,
        "Opening the tuner session.");

    ProbeController(std::move(snapshot), true);
}

void OwnershipService::CaptureConfiguration()
{
    auto snapshot = *CurrentSnapshot();
    const std::uint32_t baseAddress = snapshot.activeModuleBaseAddress;
    if (snapshot.ownership != GuidedTunerOwnershipState::SessionOpen)
    {
        Fw2051ScpConfigurationSnapshot capture;
        capture.state = ScpConfigurationState::Failed;
        capture.baseAddress = baseAddress;
        capture.message =
            "Open a tuner session before reading an SCP configuration.";
        snapshot.configurationCapture = std::move(capture);
        snapshot.configurationCompleteForTarget = false;
        snapshot.configurationFresh = false;
        RefreshProfileComparison(snapshot);
        snapshot.activeOperation = GuidedTunerOperation::None;
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Warning,
            "Open a tuner session before reading an SCP configuration.");
        return;
    }
    if (!snapshot.targetSupported)
    {
        Fw2051ScpConfigurationSnapshot capture;
        capture.state = ScpConfigurationState::Failed;
        capture.baseAddress = baseAddress;
        capture.message =
            "The selected module backend is not implemented for SCP "
            "configuration capture.";
        snapshot.configurationCapture = std::move(capture);
        snapshot.configurationCompleteForTarget = false;
        snapshot.configurationFresh = false;
        RefreshProfileComparison(snapshot);
        snapshot.activeOperation = GuidedTunerOperation::None;
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Warning,
            "The selected module backend is not implemented for SCP "
            "configuration capture.");
        return;
    }

    Fw2051ScpConfigurationSnapshot reading;
    reading.state = ScpConfigurationState::Reading;
    reading.baseAddress = baseAddress;
    reading.message =
        "Reading the global SCP settings and all eight channel quads...";
    snapshot.configurationCapture = reading;
    snapshot.configurationCompleteForTarget = false;
    snapshot.configurationFresh = false;
    snapshot.deterministicStartupPassed = false;
    snapshot.deterministicStartupResult = {};
    RefreshProfileComparison(snapshot);
    snapshot.activeOperation = GuidedTunerOperation::ConfigurationCapture;
    PublishStatus(
        std::move(snapshot),
        TunerStatusLevel::Information,
        reading.message);

    const auto operation = CaptureFw2051ScpConfiguration(
        *transport_,
        baseAddress,
        serviceStopRequested_,
        [this](const std::string& operationName) {
            return CheckCaptureOwnershipGate(operationName);
        });

    snapshot = *CurrentSnapshot();
    snapshot.activeOperation = GuidedTunerOperation::None;
    snapshot.configurationCapture = operation.configuration;
    snapshot.configurationCompleteForTarget =
        operation.configuration.state == ScpConfigurationState::Complete &&
        operation.configuration.baseAddress ==
            snapshot.activeModuleBaseAddress;
    snapshot.configurationFresh =
        snapshot.configurationCompleteForTarget;
    RefreshProfileComparison(snapshot);
    const auto level = snapshot.configurationCompleteForTarget
        ? TunerStatusLevel::Success
        : TunerStatusLevel::Error;
    const std::string message = operation.configuration.message;
    PublishStatus(std::move(snapshot), level, message);
}

void OwnershipService::SaveProfile(const std::string& path)
{
    auto snapshot = *CurrentSnapshot();
    if (!snapshot.configurationCompleteForTarget ||
        !snapshot.configurationFresh)
    {
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Warning,
            "Capture a fresh SCP configuration for the active module "
            "before saving a profile.");
        return;
    }

    const auto saved = SaveFw2051ScpProfile(
        snapshot.configurationCapture, path);
    PublishStatus(
        std::move(snapshot),
        saved.success ? TunerStatusLevel::Success : TunerStatusLevel::Error,
        saved.message);
}

void OwnershipService::LoadProfile(const std::string& path)
{
    auto snapshot = *CurrentSnapshot();
    const auto loaded = LoadFw2051ScpProfile(path);

    snapshot.profileLoaded = false;
    snapshot.profileLoadedForTarget = false;
    snapshot.loadedProfilePath.clear();
    snapshot.loadedProfile = {};
    snapshot.singleRepairResult = {};
    snapshot.bulkApplyResult = {};
    snapshot.deterministicStartupPassed = false;
    snapshot.deterministicStartupResult = {};
    snapshot.configurationComparison = {};
    snapshot.profileMatchesExactly = false;
    RefreshProfileComparison(snapshot);

    if (!loaded.success || !loaded.profile)
    {
        PublishStatus(
            std::move(snapshot), TunerStatusLevel::Error, loaded.message);
        return;
    }

    snapshot.profileLoaded = true;
    snapshot.loadedProfilePath = path;
    snapshot.loadedProfile = *loaded.profile;
    snapshot.profileLoadedForTarget = snapshot.projectActive &&
        snapshot.targetSupported &&
        snapshot.loadedProfile.configuration.baseAddress ==
            snapshot.activeModuleBaseAddress;
    RefreshProfileComparison(snapshot);

    if (!snapshot.profileLoadedForTarget)
    {
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Warning,
            "The SCP profile loaded, but its VME base does not match the "
            "active module.",
            loaded.message);
        return;
    }

    PublishStatus(
        std::move(snapshot),
        TunerStatusLevel::Success,
        loaded.message);
}

void OwnershipService::ApplyProfileRow(
    const ApplyProfileRowCommand& command)
{
    auto snapshot = *CurrentSnapshot();
    if (snapshot.ownership != GuidedTunerOwnershipState::SessionOpen)
    {
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Warning,
            "Open a tuner session before applying an SCP profile value.");
        return;
    }
    if (!snapshot.profileLoadedForTarget)
    {
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Warning,
            "Load an SCP profile for the active module before applying a "
            "value.");
        return;
    }
    if (!snapshot.configurationCompleteForTarget ||
        !snapshot.configurationFresh ||
        !snapshot.configurationComparison.comparable)
    {
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Warning,
            "Capture all eight SCP quads again before applying a profile "
            "value.");
        return;
    }

    const auto found = std::find_if(
        snapshot.configurationComparison.differences.begin(),
        snapshot.configurationComparison.differences.end(),
        [&command](const ScpConfigurationDifference& difference) {
            return difference.quad == static_cast<int>(command.quad) &&
                difference.hasRegister &&
                difference.registerOffset == command.registerOffset;
        });
    if (found == snapshot.configurationComparison.differences.end() ||
        FindFw2051ScpSetting(command.registerOffset) == nullptr ||
        found->liveValue > 0xFFFFU || found->profileValue > 0xFFFFU)
    {
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Warning,
            "The requested row is not an applicable banked SCP profile "
            "difference.");
        return;
    }

    ScpSingleRepairRequest request;
    request.baseAddress = snapshot.activeModuleBaseAddress;
    request.quad = command.quad;
    request.registerOffset = command.registerOffset;
    request.expectedLiveValue = static_cast<std::uint16_t>(found->liveValue);
    request.profileValue = static_cast<std::uint16_t>(found->profileValue);

    snapshot.activeOperation = GuidedTunerOperation::ProfileApplication;
    snapshot.singleRepairResult = {};
    snapshot.singleRepairResult.state = ScpSingleRepairState::Applying;
    snapshot.singleRepairResult.message =
        "Rechecking the selected live register before one profile write...";
    PublishStatus(
        std::move(snapshot),
        TunerStatusLevel::Information,
        "Applying one banked SCP profile difference.");

    const auto result = RepairFw2051ScpProfileValue(
        *transport_,
        request,
        serviceStopRequested_,
        [this](const std::string& operationName) {
            return CheckApplyOwnershipGate(operationName);
        });

    snapshot = *CurrentSnapshot();
    snapshot.activeOperation = GuidedTunerOperation::None;
    snapshot.singleRepairResult = result;
    snapshot.configurationFresh = false;
    snapshot.deterministicStartupPassed = false;
    snapshot.deterministicStartupResult = {};
    RefreshProfileComparison(snapshot);
    PublishStatus(
        std::move(snapshot),
        result.state == ScpSingleRepairState::Passed
            ? TunerStatusLevel::Success
            : TunerStatusLevel::Error,
        result.message);
}

void OwnershipService::ApplyAllDifferences()
{
    auto snapshot = *CurrentSnapshot();
    if (snapshot.ownership != GuidedTunerOwnershipState::SessionOpen)
    {
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Warning,
            "Open a tuner session before applying an SCP profile.");
        return;
    }
    if (!snapshot.profileLoadedForTarget)
    {
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Warning,
            "Load an SCP profile for the active module before applying it.");
        return;
    }
    if (!snapshot.configurationCompleteForTarget ||
        !snapshot.configurationFresh ||
        !snapshot.configurationComparison.comparable)
    {
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Warning,
            "Capture all eight SCP quads again before applying a profile.");
        return;
    }

    RefreshProfileComparison(snapshot);
    if (!snapshot.profileApplicationPlan.success ||
        snapshot.profileApplicationPlan.request.steps.empty())
    {
        const auto message = snapshot.profileApplicationPlan.message.empty()
            ? "There are no applicable banked SCP differences to apply."
            : snapshot.profileApplicationPlan.message;
        PublishStatus(
            std::move(snapshot), TunerStatusLevel::Warning, message);
        return;
    }

    const auto request = snapshot.profileApplicationPlan.request;
    snapshot.activeOperation = GuidedTunerOperation::ProfileApplication;
    snapshot.bulkApplyResult = {};
    snapshot.bulkApplyResult.state = ScpBulkApplyState::Applying;
    snapshot.bulkApplyResult.plannedWrites = request.steps.size();
    snapshot.bulkApplyResult.message =
        "Rereading all 140 hardware values before the first profile write...";
    PublishStatus(
        std::move(snapshot),
        TunerStatusLevel::Information,
        "Applying all planned banked SCP profile differences.");

    const auto result = ApplyFw2051ScpProfile(
        *transport_,
        request,
        serviceStopRequested_,
        [this](const std::string& operationName) {
            return CheckApplyOwnershipGate(operationName);
        });

    snapshot = *CurrentSnapshot();
    snapshot.activeOperation = GuidedTunerOperation::None;
    snapshot.bulkApplyResult = result;
    snapshot.configurationFresh = false;
    snapshot.deterministicStartupPassed = false;
    snapshot.deterministicStartupResult = {};
    RefreshProfileComparison(snapshot);
    PublishStatus(
        std::move(snapshot),
        result.state == ScpBulkApplyState::Passed
            ? TunerStatusLevel::Success
            : TunerStatusLevel::Error,
        result.message);
}

void OwnershipService::RunDeterministicStartup(
    const RunDeterministicStartupCommand& command)
{
    auto snapshot = *CurrentSnapshot();
    RefreshProfileComparison(snapshot);
    if (snapshot.ownership != GuidedTunerOwnershipState::SessionOpen)
    {
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Warning,
            "Open a tuner session before deterministic startup.");
        return;
    }
    if (!command.confirmed)
    {
        snapshot.deterministicStartupResult = {};
        snapshot.deterministicStartupResult.state =
            DeterministicStartupState::Failed;
        snapshot.deterministicStartupResult.message =
            "Deterministic startup requires explicit confirmation of the "
            "reviewed recipe.";
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Warning,
            "Deterministic startup requires explicit confirmation of the "
            "reviewed recipe.");
        return;
    }
    if (!snapshot.startupPlanAvailable ||
        !snapshot.standaloneStartupPlan.success)
    {
        const auto message = snapshot.standaloneStartupPlan.message.empty()
            ? "The deterministic startup recipe is not available."
            : snapshot.standaloneStartupPlan.message;
        PublishStatus(
            std::move(snapshot), TunerStatusLevel::Warning, message);
        return;
    }

    DeterministicStartupRequest request;
    request.profileLoadedForTarget = snapshot.profileLoadedForTarget;
    request.configurationFresh = snapshot.configurationFresh;
    request.startupAuditCompleteForTarget =
        snapshot.startupAuditCompleteForTarget;
    request.confirmed = command.confirmed;
    request.profile = snapshot.loadedProfile;
    request.reviewedConfiguration = snapshot.configurationCapture;
    request.startupAudit = snapshot.startupAudit;

    snapshot.activeOperation = GuidedTunerOperation::StartupPreparation;
    snapshot.configurationFresh = false;
    snapshot.deterministicStartupPassed = false;
    snapshot.deterministicStartupResult = {};
    snapshot.deterministicStartupResult.state =
        DeterministicStartupState::PreparingReadout;
    snapshot.deterministicStartupResult.baseAddress =
        snapshot.activeModuleBaseAddress;
    snapshot.deterministicStartupResult.message =
        "Running the strict deterministic startup sequence...";
    RefreshProfileComparison(snapshot);
    PublishStatus(
        std::move(snapshot),
        TunerStatusLevel::Information,
        "Running the strict deterministic startup sequence.");

    const auto result = RunFw2051DeterministicStartup(
        *transport_,
        request,
        serviceStopRequested_,
        [this](const std::string& operationName) {
            return CheckStartupOwnershipGate(operationName);
        });

    snapshot = *CurrentSnapshot();
    snapshot.activeOperation = GuidedTunerOperation::None;
    snapshot.deterministicStartupResult = result;
    snapshot.deterministicStartupPassed =
        result.state == DeterministicStartupState::Passed &&
        result.finalProfileVerified && result.moduleLeftStopped;
    if (snapshot.deterministicStartupPassed)
    {
        snapshot.configurationCapture = result.finalConfiguration;
        snapshot.configurationCompleteForTarget =
            result.finalConfiguration.state ==
                ScpConfigurationState::Complete &&
            result.finalConfiguration.baseAddress ==
                snapshot.activeModuleBaseAddress;
        snapshot.configurationFresh =
            snapshot.configurationCompleteForTarget;
    }
    else
    {
        snapshot.configurationFresh = false;
    }
    RefreshProfileComparison(snapshot);
    PublishStatus(
        std::move(snapshot),
        result.state == DeterministicStartupState::Passed
            ? TunerStatusLevel::Success
            : TunerStatusLevel::Error,
        result.message);
}

void OwnershipService::ProbeController(
    TunerSnapshot snapshot,
    bool retainSession)
{

    transport_->Close();
    const auto opened = transport_->Open(
        snapshot.mvlcHost, snapshot.mvlcCommandPort);
    if (!opened.success)
    {
        transport_->Close();
        snapshot.ownership = GuidedTunerOwnershipState::Failed;
        PublishStatus(
            std::move(snapshot), TunerStatusLevel::Error, opened.error);
        return;
    }

    nextReadReference_ = 1U;
    const auto firmware = ReadLocalRegister(
        FirmwareRevisionRegister,
        nextReadReference_++,
        serviceStopRequested_);
    if (!firmware.success)
    {
        transport_->Close();
        snapshot.ownership = GuidedTunerOwnershipState::Failed;
        PublishStatus(
            std::move(snapshot), TunerStatusLevel::Error, firmware.error);
        return;
    }
    snapshot.mvlcFirmwareRevision = firmware.value;

    const auto daq = ReadLocalRegister(
        DaqModeRegister, nextReadReference_++, serviceStopRequested_);
    if (!daq.success)
    {
        transport_->Close();
        snapshot.ownership = GuidedTunerOwnershipState::Failed;
        PublishStatus(
            std::move(snapshot), TunerStatusLevel::Error, daq.error);
        return;
    }
    snapshot.mvlcDaqMode = daq.value;
    if (daq.value != 0U)
    {
        transport_->Close();
        snapshot.ownership = GuidedTunerOwnershipState::InUse;
        PublishStatus(
            std::move(snapshot), TunerStatusLevel::Warning, InUseMessage);
        return;
    }

    const auto hardware = ReadLocalRegister(
        HardwareIdRegister,
        nextReadReference_++,
        serviceStopRequested_);
    if (!retainSession)
    {
        transport_->Close();
    }
    if (!hardware.success)
    {
        transport_->Close();
        snapshot.ownership = GuidedTunerOwnershipState::Failed;
        PublishStatus(
            std::move(snapshot), TunerStatusLevel::Error, hardware.error);
        return;
    }
    snapshot.mvlcHardwareId = hardware.value;
    snapshot.controllerReadingsValid = true;
    if (hardware.value != ExpectedMvlcHardwareId)
    {
        transport_->Close();
        snapshot.ownership = GuidedTunerOwnershipState::Failed;
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Error,
            InvalidHardwareMessage);
        return;
    }

    snapshot.ownership = retainSession
        ? GuidedTunerOwnershipState::SessionOpen
        : GuidedTunerOwnershipState::Idle;
    watchdogCommunicationUncertain_ = false;
    PublishStatus(
        std::move(snapshot),
        TunerStatusLevel::Success,
        retainSession ? SessionOpenMessage : IdleMessage);
    if (!retainSession)
    {
        return;
    }

    nextWatchdogReference_ = 0x7000U;
    try
    {
        StartWatchdog();
    }
    catch (const std::system_error& error)
    {
        transport_->Close();
        auto failed = *CurrentSnapshot();
        failed.ownership = GuidedTunerOwnershipState::Failed;
        PublishStatus(
            std::move(failed),
            TunerStatusLevel::Error,
            "The ownership watchdog could not start.",
            error.what());
    }
}

void OwnershipService::ReleaseSession()
{
    if (transport_)
    {
        transport_->Close();
    }
    watchdogCommunicationUncertain_ = false;

    auto snapshot = *CurrentSnapshot();
    snapshot.ownership = GuidedTunerOwnershipState::Disconnected;
    snapshot.controllerReadingsValid = false;
    snapshot.mvlcHardwareId = 0U;
    snapshot.mvlcFirmwareRevision = 0U;
    snapshot.mvlcDaqMode = 0U;
    snapshot.mvmeHandoffConfirmed = false;
    snapshot.activeOperation = GuidedTunerOperation::None;
    snapshot.startupAuditCompleteForTarget = false;
    snapshot.startupAuditReady = false;
    snapshot.startupAudit = {};
    snapshot.profileLoaded = false;
    snapshot.profileLoadedForTarget = false;
    snapshot.loadedProfilePath.clear();
    snapshot.loadedProfile = {};
    snapshot.configurationCompleteForTarget = false;
    snapshot.configurationFresh = false;
    snapshot.configurationCapture = {};
    snapshot.configurationComparison = {};
    snapshot.profileApplicationPlan = {};
    snapshot.profileMatchesExactly = false;
    snapshot.singleRepairResult = {};
    snapshot.bulkApplyResult = {};
    snapshot.startupPlanAvailable = false;
    snapshot.standaloneStartupPlan = {};
    snapshot.startupPreparationMismatches.clear();
    snapshot.deterministicStartupPassed = false;
    snapshot.deterministicStartupResult = {};
    PublishStatus(
        std::move(snapshot),
        TunerStatusLevel::Information,
        DisconnectedMessage);
}

void OwnershipService::RunStartupAudit()
{
    auto snapshot = *CurrentSnapshot();
    const std::uint32_t baseAddress = snapshot.activeModuleBaseAddress;
    if (snapshot.ownership != GuidedTunerOwnershipState::SessionOpen)
    {
        StartupAuditResult audit;
        audit.state = StartupAuditState::Failed;
        audit.baseAddress = baseAddress;
        audit.message =
            "Open a tuner session before auditing module-wide startup "
            "settings.";
        snapshot.startupAudit = std::move(audit);
        snapshot.startupAuditCompleteForTarget = false;
        snapshot.startupAuditReady = false;
        snapshot.activeOperation = GuidedTunerOperation::None;
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Warning,
            "Open a tuner session before auditing module-wide startup "
            "settings.");
        return;
    }
    if (!snapshot.targetSupported)
    {
        StartupAuditResult audit;
        audit.state = StartupAuditState::Failed;
        audit.baseAddress = baseAddress;
        audit.message =
            "The selected module backend is not implemented for startup "
            "auditing.";
        snapshot.startupAudit = std::move(audit);
        snapshot.startupAuditCompleteForTarget = false;
        snapshot.startupAuditReady = false;
        snapshot.activeOperation = GuidedTunerOperation::None;
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Warning,
            "The selected module backend is not implemented for startup "
            "auditing.");
        return;
    }

    StartupAuditResult audit;
    audit.state = StartupAuditState::Reading;
    audit.baseAddress = baseAddress;
    audit.message =
        "Reading module-wide startup settings without issuing a VME "
        "write...";
    audit.vmeWritesIssued = false;
    audit.rows.reserve(Fw2051StartupAuditRegisterCount);
    snapshot.startupAudit = audit;
    snapshot.startupAuditCompleteForTarget = false;
    snapshot.startupAuditReady = false;
    snapshot.deterministicStartupPassed = false;
    snapshot.deterministicStartupResult = {};
    RefreshProfileComparison(snapshot);
    snapshot.activeOperation = GuidedTunerOperation::Audit;
    PublishStatus(
        std::move(snapshot),
        TunerStatusLevel::Information,
        audit.message);

    const auto gate = CheckPreWriteGate("running the startup audit");
    if (!gate.allowed)
    {
        snapshot = *CurrentSnapshot();
        audit.state = StartupAuditState::Failed;
        audit.message = gate.message;
        snapshot.startupAudit = std::move(audit);
        snapshot.startupAuditCompleteForTarget = false;
        snapshot.startupAuditReady = false;
        snapshot.activeOperation = GuidedTunerOperation::None;
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Error,
            gate.message);
        return;
    }

    nextAuditSuperReference_ = 0x1600U;
    nextAuditStackReference_ = 0x9C080001U;
    std::array<std::uint16_t, Fw2051StartupAuditRegisterCount> values{};
    std::string failure;

    for (std::size_t index = 0U;
         index < Fw2051StartupAuditRegisterTable.size(); ++index)
    {
        const auto& definition = Fw2051StartupAuditRegisterTable[index];
        const auto read = ReadVmeD16(
            *transport_,
            baseAddress + definition.registerOffset,
            nextAuditSuperReference_,
            nextAuditStackReference_,
            serviceStopRequested_);
        if (!read.success)
        {
            char registerText[16]{};
            std::snprintf(
                registerText,
                sizeof(registerText),
                "%04X",
                static_cast<unsigned>(definition.registerOffset));
            failure =
                std::string("Could not read ") + definition.name +
                " at register 0x" + registerText + ": " + read.error;
            break;
        }

        values[index] = read.value;
        audit.rows.push_back(StartupAuditRow{
            definition.registerOffset,
            definition.name,
            definition.group,
            read.value,
            definition.role,
            StartupAuditAssessment::Inherited,
            {}});
    }

    snapshot = *CurrentSnapshot();
    snapshot.activeOperation = GuidedTunerOperation::None;
    if (!failure.empty())
    {
        audit.state = StartupAuditState::Failed;
        audit.message = std::move(failure);
        audit.registersRead = audit.rows.size();
        snapshot.startupAudit = std::move(audit);
        snapshot.startupAuditCompleteForTarget = false;
        snapshot.startupAuditReady = false;
        const std::string message = snapshot.startupAudit.message;
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Error,
            message);
        return;
    }

    audit = ClassifyFw2051StartupAudit(baseAddress, values);
    snapshot.startupAudit = audit;
    snapshot.startupAuditCompleteForTarget =
        audit.state == StartupAuditState::Complete &&
        audit.baseAddress == snapshot.activeModuleBaseAddress;
    snapshot.startupAuditReady =
        snapshot.startupAuditCompleteForTarget &&
        audit.readyForDiagnosticStart;
    RefreshProfileComparison(snapshot);
    PublishStatus(
        std::move(snapshot),
        audit.readyForDiagnosticStart
            ? TunerStatusLevel::Success
            : TunerStatusLevel::Warning,
        audit.message);
}

OwnershipService::LocalReadResult OwnershipService::ReadLocalRegister(
    std::uint16_t address,
    std::uint16_t reference,
    const std::atomic<bool>& cancelled)
{
    LocalReadResult result;
    const auto words = BuildMvlcLocalRegisterReadRequest(reference, address);
    const auto request = EncodeMvlcWordsLittleEndian(words.data(), words.size());
    std::array<std::byte, CommandResponseBufferSize> response{};

    for (int attempt = 0;
         attempt < ReadOnlyTransactionAttempts && !cancelled.load();
         ++attempt)
    {
        const auto sent = transport_->Send(request.data(), request.size());
        if (!sent.success)
        {
            result.error = sent.error;
            return result;
        }

        for (int datagram = 0;
             datagram < MaximumReadOnlyResponseDatagrams
                 && !cancelled.load();
             ++datagram)
        {
            const auto received = transport_->Receive(
                response.data(),
                response.size(),
                CommandReceiveTimeoutMilliseconds);
            if (received.status == TransportReceiveStatus::Timeout)
            {
                break;
            }
            if (received.status == TransportReceiveStatus::Error)
            {
                result.error = received.error;
                return result;
            }

            const auto parsed = ParseMvlcLocalRegisterReadReply(
                response.data(),
                received.bytesReceived,
                reference,
                address);
            if (parsed.status == MvlcLocalReadReplyStatus::Match)
            {
                result.success = true;
                result.value = parsed.value;
                return result;
            }
            if (parsed.status == MvlcLocalReadReplyStatus::Malformed)
            {
                result.error = "receive: malformed MVLC Ethernet response";
                return result;
            }
        }
    }

    result.error = cancelled.load()
        ? "Preflight cancelled"
        : "No MVLC response after three read-only attempts";
    return result;
}

PreWriteGateResult OwnershipService::CheckPreWriteGate(
    const std::string& operationName)
{
    auto snapshot = *CurrentSnapshot();
    if (snapshot.ownership != GuidedTunerOwnershipState::SessionOpen)
    {
        return {false, "No open tuner session is available."};
    }

    const auto daq = ReadLocalRegister(
        DaqModeRegister, nextReadReference_++, serviceStopRequested_);
    if (!daq.success)
    {
        const std::string message =
            "The operation was cancelled without a hardware write because "
            "MVLC command communication is temporarily uncertain before "
            + operationName + ". The session remains open and the watchdog "
            "will retry: " + daq.error;
        PublishStatus(
            std::move(snapshot), TunerStatusLevel::Warning, message);
        return {false, message};
    }

    if (daq.value != 0U)
    {
        const std::string message =
            "A DAQ became active before " + operationName
            + ". The tuner released its command socket without touching "
              "the MDPP, DAQ mode, or readout stacks.";
        DetachForForeignDaq(daq.value, message);
        return {false, message};
    }

    snapshot.mvlcDaqMode = daq.value;
    Publish(std::move(snapshot));
    return {true, {}};
}

ScpCaptureGateResult OwnershipService::CheckCaptureOwnershipGate(
    const std::string& operationName)
{
    auto snapshot = *CurrentSnapshot();
    if (serviceStopRequested_.load())
    {
        return {
            ScpCaptureGateStatus::Cancelled,
            "SCP configuration capture was cancelled.",
        };
    }
    if (snapshot.ownership != GuidedTunerOwnershipState::SessionOpen)
    {
        return {
            snapshot.ownership == GuidedTunerOwnershipState::OwnershipLost
                ? ScpCaptureGateStatus::OwnershipLost
                : ScpCaptureGateStatus::CommunicationUnavailable,
            "No open tuner session is available.",
        };
    }

    const auto daq = ReadLocalRegister(
        DaqModeRegister, nextReadReference_++, serviceStopRequested_);
    if (!daq.success)
    {
        const std::string message =
            "SCP configuration capture stopped because MVLC command "
            "communication is temporarily uncertain before " +
            operationName + ". No further selector write was sent. The "
            "session remains open and the watchdog will retry: " +
            daq.error;
        PublishStatus(
            std::move(snapshot), TunerStatusLevel::Warning, message);
        return {
            serviceStopRequested_.load()
                ? ScpCaptureGateStatus::Cancelled
                : ScpCaptureGateStatus::CommunicationUnavailable,
            message,
        };
    }

    if (daq.value != 0U)
    {
        const std::string message =
            "A DAQ became active before " + operationName +
            ". The tuner passively detached and sent no further MDPP or "
            "readout-stack write.";
        DetachForForeignDaq(daq.value, message);
        return {ScpCaptureGateStatus::OwnershipLost, message};
    }

    snapshot.mvlcDaqMode = daq.value;
    Publish(std::move(snapshot));
    return {ScpCaptureGateStatus::Allowed, {}};
}

ScpCaptureGateResult OwnershipService::CheckApplyOwnershipGate(
    const std::string& operationName)
{
    auto snapshot = *CurrentSnapshot();
    if (serviceStopRequested_.load())
    {
        return {
            ScpCaptureGateStatus::Cancelled,
            "The SCP profile transaction was cancelled.",
        };
    }
    if (snapshot.ownership != GuidedTunerOwnershipState::SessionOpen)
    {
        return {
            snapshot.ownership == GuidedTunerOwnershipState::OwnershipLost
                ? ScpCaptureGateStatus::OwnershipLost
                : ScpCaptureGateStatus::CommunicationUnavailable,
            "No open tuner session is available.",
        };
    }

    const auto daq = ReadLocalRegister(
        DaqModeRegister, nextReadReference_++, serviceStopRequested_);
    if (!daq.success)
    {
        const std::string message =
            "The SCP profile transaction stopped because MVLC command "
            "communication is temporarily uncertain before " +
            operationName + ". No further hardware write was sent. The "
            "session remains open and the watchdog will retry: " +
            daq.error;
        PublishStatus(
            std::move(snapshot), TunerStatusLevel::Warning, message);
        return {
            serviceStopRequested_.load()
                ? ScpCaptureGateStatus::Cancelled
                : ScpCaptureGateStatus::CommunicationUnavailable,
            message,
        };
    }

    if (daq.value != 0U)
    {
        const std::string message =
            "A DAQ became active before " + operationName +
            ". The tuner passively detached and sent no further MDPP or "
            "readout-stack write.";
        DetachForForeignDaq(daq.value, message);
        return {ScpCaptureGateStatus::OwnershipLost, message};
    }

    snapshot.mvlcDaqMode = daq.value;
    Publish(std::move(snapshot));
    return {ScpCaptureGateStatus::Allowed, {}};
}

ScpCaptureGateResult OwnershipService::CheckStartupOwnershipGate(
    const std::string& operationName)
{
    auto snapshot = *CurrentSnapshot();
    if (serviceStopRequested_.load())
    {
        return {
            ScpCaptureGateStatus::Cancelled,
            "Deterministic startup was cancelled.",
        };
    }
    if (snapshot.ownership != GuidedTunerOwnershipState::SessionOpen)
    {
        return {
            snapshot.ownership == GuidedTunerOwnershipState::OwnershipLost
                ? ScpCaptureGateStatus::OwnershipLost
                : ScpCaptureGateStatus::CommunicationUnavailable,
            "No open tuner session is available.",
        };
    }

    const auto daq = ReadLocalRegister(
        DaqModeRegister, nextReadReference_++, serviceStopRequested_);
    if (!daq.success)
    {
        const std::string message =
            "Deterministic startup stopped because MVLC command "
            "communication is temporarily uncertain before " +
            operationName + ". No further hardware write was sent. The "
            "session remains open and the watchdog will retry: " +
            daq.error;
        PublishStatus(
            std::move(snapshot), TunerStatusLevel::Warning, message);
        return {
            serviceStopRequested_.load()
                ? ScpCaptureGateStatus::Cancelled
                : ScpCaptureGateStatus::CommunicationUnavailable,
            message,
        };
    }

    if (daq.value != 0U)
    {
        const std::string message =
            "A DAQ became active before " + operationName +
            ". The tuner passively detached and sent no further MDPP or "
            "readout-stack write.";
        DetachForForeignDaq(daq.value, message);
        return {ScpCaptureGateStatus::OwnershipLost, message};
    }

    snapshot.mvlcDaqMode = daq.value;
    Publish(std::move(snapshot));
    return {ScpCaptureGateStatus::Allowed, {}};
}

void OwnershipService::RefreshProfileComparison(TunerSnapshot& snapshot)
{
    snapshot.configurationComparison = {};
    snapshot.profileApplicationPlan = {};
    snapshot.standaloneStartupPlan = {};
    snapshot.startupPlanAvailable = false;
    snapshot.startupPreparationMismatches.clear();
    snapshot.profileMatchesExactly = false;

    if (snapshot.startupAuditCompleteForTarget)
    {
        std::array<
            std::uint16_t,
            Fw2051StartupPreparationRegisterCount> values{};
        bool foundEveryValue = true;
        for (std::size_t index = 0U;
             index < Fw2051StartupPreparationRegisterTable.size();
             ++index)
        {
            const auto registerOffset =
                Fw2051StartupPreparationRegisterTable[index]
                    .registerOffset;
            const auto found = std::find_if(
                snapshot.startupAudit.rows.begin(),
                snapshot.startupAudit.rows.end(),
                [registerOffset](const StartupAuditRow& row) {
                    return row.registerOffset == registerOffset;
                });
            if (found == snapshot.startupAudit.rows.end())
            {
                foundEveryValue = false;
                break;
            }
            values[index] = found->value;
        }
        if (foundEveryValue)
        {
            snapshot.startupPreparationMismatches =
                FindFw2051StartupPreparationMismatches(values);
        }
    }
    if (!snapshot.profileLoadedForTarget)
    {
        snapshot.configurationComparison.message = snapshot.profileLoaded
            ? "The loaded SCP profile is for a different VME base."
            : "No SCP profile is loaded for the active module.";
        return;
    }
    if (!snapshot.configurationCompleteForTarget ||
        !snapshot.configurationFresh)
    {
        snapshot.configurationComparison.message =
            "Capture a fresh SCP configuration before comparing it with "
            "the loaded profile.";
        return;
    }

    snapshot.configurationComparison = CompareFw2051ScpConfiguration(
        snapshot.loadedProfile, snapshot.configurationCapture);
    snapshot.profileMatchesExactly =
        snapshot.configurationComparison.comparable &&
        snapshot.configurationComparison.differences.empty();
    if (snapshot.configurationComparison.comparable)
    {
        snapshot.profileApplicationPlan =
            PlanFw2051ScpProfileApplication(
                snapshot.loadedProfile,
                snapshot.configurationCapture);
        snapshot.standaloneStartupPlan =
            PlanFw2051ScpStandaloneStartup(
                snapshot.loadedProfile,
                snapshot.configurationCapture);
        snapshot.startupPlanAvailable =
            snapshot.standaloneStartupPlan.success &&
            snapshot.startupAuditCompleteForTarget &&
            snapshot.startupAudit.hardwareId == Mdpp32HardwareId &&
            snapshot.startupAudit.firmwareRevision ==
                Mdpp32ScpFirmwareRevisionFw2051;
    }
}

void OwnershipService::StartWatchdog()
{
    watchdogStopRequested_.store(false);
    watchdog_ = std::thread(&OwnershipService::WatchdogLoop, this);
}

void OwnershipService::StopWatchdog()
{
    RequestWatchdogStop();
    if (watchdog_.joinable())
    {
        watchdog_.join();
    }
}

void OwnershipService::RequestWatchdogStop()
{
    watchdogStopRequested_.store(true);
    watchdogWakeup_.notify_all();
}

void OwnershipService::WatchdogLoop()
{
    std::unique_lock<std::mutex> lock(watchdogMutex_);
    while (!watchdogStopRequested_.load())
    {
        if (watchdogWakeup_.wait_for(
                lock,
                watchdogInterval_,
                [this] { return watchdogStopRequested_.load(); }))
        {
            break;
        }

        lock.unlock();
        auto poll = worker_.Post([this] { PollWatchdog(); });
        try
        {
            poll.get();
        }
        catch (const std::exception&)
        {
            RequestWatchdogStop();
        }
        lock.lock();
    }
}

void OwnershipService::PollWatchdog()
{
    auto snapshot = *CurrentSnapshot();
    if (watchdogStopRequested_.load()
        || snapshot.ownership != GuidedTunerOwnershipState::SessionOpen)
    {
        RequestWatchdogStop();
        return;
    }

    const auto daq = ReadLocalRegister(
        DaqModeRegister,
        nextWatchdogReference_++,
        watchdogStopRequested_);
    if (!daq.success)
    {
        if (!watchdogStopRequested_.load())
        {
            watchdogCommunicationUncertain_ = true;
            PublishStatus(
                std::move(snapshot),
                TunerStatusLevel::Warning,
                "MVLC command communication is temporarily uncertain. No "
                "hardware operation is allowed until a later watchdog read "
                "succeeds: " + daq.error);
        }
        return;
    }

    if (daq.value != 0U)
    {
        DetachForForeignDaq(
            daq.value,
            "Foreign DAQ activity was detected while the tuner was idle. "
            "The tuner released its command socket without stopping the "
            "MDPP, changing DAQ mode, or clearing any readout stack.");
        return;
    }

    snapshot.mvlcDaqMode = daq.value;
    if (watchdogCommunicationUncertain_)
    {
        watchdogCommunicationUncertain_ = false;
        PublishStatus(
            std::move(snapshot),
            TunerStatusLevel::Success,
            "MVLC command communication recovered; DAQ mode is still idle "
            "and controlled operations are available again.");
        return;
    }
    Publish(std::move(snapshot));
}

void OwnershipService::DetachForForeignDaq(
    std::uint32_t daqMode,
    std::string message)
{
    RequestWatchdogStop();
    transport_->Close();

    auto snapshot = *CurrentSnapshot();
    snapshot.ownership = GuidedTunerOwnershipState::OwnershipLost;
    snapshot.mvlcDaqMode = daqMode;
    snapshot.activeOperation = GuidedTunerOperation::None;
    snapshot.startupAuditCompleteForTarget = false;
    snapshot.startupAuditReady = false;
    snapshot.startupAudit = {};
    snapshot.configurationCompleteForTarget = false;
    snapshot.configurationFresh = false;
    snapshot.deterministicStartupPassed = false;
    snapshot.deterministicStartupResult = {};
    RefreshProfileComparison(snapshot);
    PublishStatus(
        std::move(snapshot),
        TunerStatusLevel::Error,
        std::move(message));
}

void OwnershipService::Publish(TunerSnapshot snapshot)
{
    const auto current = CurrentSnapshot();
    snapshot.revision = current ? current->revision + 1U : 1U;
    std::shared_ptr<const TunerSnapshot> published =
        std::make_shared<const TunerSnapshot>(std::move(snapshot));
    std::atomic_store_explicit(
        &snapshot_, std::move(published), std::memory_order_release);
}

void OwnershipService::PublishStatus(
    TunerSnapshot snapshot,
    TunerStatusLevel level,
    std::string summary,
    std::string detail)
{
    snapshot.statusMessages.clear();
    snapshot.statusMessages.push_back(
        {level, std::move(summary), std::move(detail)});
    Publish(std::move(snapshot));
}

} // namespace fidget
