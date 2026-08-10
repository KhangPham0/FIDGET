#include "hardware/OwnershipService.h"

#include "core/CrateProject.h"
#include "core/VmeProtocol.h"

#include <array>
#include <cstddef>
#include <exception>
#include <system_error>
#include <utility>

namespace fidget {
namespace {

constexpr int ReadOnlyTransactionAttempts = 3;
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
    PublishStatus(
        snapshot,
        TunerStatusLevel::Information,
        "Opening the tuner session.");

    ProbeController(std::move(snapshot), true);
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
    PublishStatus(
        std::move(snapshot),
        TunerStatusLevel::Information,
        DisconnectedMessage);
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

        const auto received = transport_->Receive(
            response.data(),
            response.size(),
            CommandReceiveTimeoutMilliseconds);
        if (received.status == TransportReceiveStatus::Timeout)
        {
            continue;
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
