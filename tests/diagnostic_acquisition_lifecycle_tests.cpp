#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/RecoveryJournal.h"
#include "core/VmeProtocol.h"
#include "hardware/AcquisitionReceiver.h"
#include "hardware/DiagnosticAcquisitionOperation.h"
#include "hardware/MvlcCommandTransport.h"
#include "hardware/MvlcDataReceiver.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <mutex>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

#include <arpa/inet.h>

namespace {

constexpr std::uint32_t TargetBase = 0x11000000U;
constexpr std::uint32_t OwnershipToken = 0xA55A1234U;

class JournalPath
{
public:
    JournalPath()
    {
        const auto unique = std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count();
        path_ = (std::filesystem::temp_directory_path()
                 / ("fidget-lifecycle-" + std::to_string(unique)
                    + ".recovery"))
                    .string();
    }

    ~JournalPath()
    {
        std::string error;
        static_cast<void>(fidget::RemoveTunerRecoveryJournal(path_, error));
    }

    [[nodiscard]] const std::string& Get() const noexcept
    {
        return path_;
    }

private:
    std::string path_;
};

std::uint32_t EncodeSigned14(const int value)
{
    return static_cast<std::uint32_t>(value) & 0x3FFFU;
}

std::uint32_t PackSamples(const int even, const int odd)
{
    return 0x30000000U | EncodeSigned14(even)
        | (EncodeSigned14(odd) << 14U);
}

std::vector<std::byte> MakeWaveformPacket(
    const std::uint16_t packetNumber,
    const int sampleValue)
{
    const std::array<std::uint32_t, 10> words{{
        0x20000008U
            | (static_cast<std::uint32_t>(packetNumber) << 16U),
        0x00000000U,
        0xF3010007U,
        0xF5200006U,
        0x41110005U,
        0x10000000U | (29U << 16U),
        0x30080001U,
        PackSamples(sampleValue, -sampleValue),
        0x20000000U,
        0xC0000000U | packetNumber,
    }};
    return fidget::EncodeMvlcWordsLittleEndian(
        words.data(), words.size());
}

std::vector<std::byte> MakeCommandPacket(
    const std::vector<std::vector<std::uint32_t>>& frames)
{
    std::vector<std::uint32_t> words{0U, 0U};
    for (const auto& frame : frames)
    {
        words.insert(words.end(), frame.begin(), frame.end());
    }
    words[0] = static_cast<std::uint32_t>(words.size() - 2U);
    return fidget::EncodeMvlcWordsLittleEndian(
        words.data(), words.size());
}

std::vector<std::uint32_t> SuperFrame(const std::uint16_t reference)
{
    return {
        (static_cast<std::uint32_t>(fidget::MvlcSuperFrameType) << 24U)
            | 1U,
        fidget::MvlcReferenceWordCommand | reference,
    };
}

class LocalMvlcEmulator
{
public:
    explicit LocalMvlcEmulator(std::string journalPath)
        : journalPath_(std::move(journalPath))
    {
        OpenAdjacentSockets();
        vmeRegisters_[TargetBase + fidget::DiagnosticHardwareIdRegister]
            = 0x5007U;
        vmeRegisters_[TargetBase + fidget::DiagnosticOutputFormatRegister]
            = 0x0018U;
        vmeRegisters_[TargetBase + fidget::DiagnosticIrqLevelRegister]
            = 3U;
        vmeRegisters_[
            TargetBase + fidget::DiagnosticAcquisitionControlRegister]
            = 0U;
        thread_ = std::thread(&LocalMvlcEmulator::Run, this);
    }

    ~LocalMvlcEmulator()
    {
        stop_.store(true);
        if (thread_.joinable())
        {
            thread_.join();
        }
        if (commandSocket_ >= 0)
        {
            ::close(commandSocket_);
        }
        if (dataSocket_ >= 0)
        {
            ::close(dataSocket_);
        }
    }

    LocalMvlcEmulator(const LocalMvlcEmulator&) = delete;
    LocalMvlcEmulator& operator=(const LocalMvlcEmulator&) = delete;

    [[nodiscard]] std::uint16_t CommandPort() const noexcept
    {
        return commandPort_;
    }

    [[nodiscard]] std::string Error() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return error_;
    }

    [[nodiscard]] std::uint32_t LocalRegister(
        const std::uint16_t address) const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = localRegisters_.find(address);
        return found == localRegisters_.end() ? 0U : found->second;
    }

    [[nodiscard]] std::uint16_t VmeRegister(
        const std::uint32_t address) const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = vmeRegisters_.find(address);
        return found == vmeRegisters_.end() ? 0U : found->second;
    }

    [[nodiscard]] bool FirstStackWriteSawJournal() const noexcept
    {
        return firstStackWriteSawJournal_.load();
    }

    [[nodiscard]] bool FirstStackWriteSeen() const noexcept
    {
        return firstStackWriteSeen_.load();
    }

private:
    enum class PendingVmeKind
    {
        None,
        Read,
        Write,
    };

    struct PendingVmeOperation
    {
        PendingVmeKind kind = PendingVmeKind::None;
        std::uint32_t stackReference = 0U;
        std::uint32_t address = 0U;
        std::uint16_t value = 0U;
    };

    static int OpenBoundSocket(const std::uint16_t port)
    {
        const int descriptor = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (descriptor < 0)
        {
            return -1;
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        if (::bind(
                descriptor,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) != 0)
        {
            ::close(descriptor);
            return -1;
        }
        return descriptor;
    }

    void OpenAdjacentSockets()
    {
        for (int attempt = 0; attempt < 100; ++attempt)
        {
            commandSocket_ = OpenBoundSocket(0U);
            REQUIRE(commandSocket_ >= 0);

            sockaddr_in address{};
            socklen_t addressLength = sizeof(address);
            REQUIRE(::getsockname(
                commandSocket_,
                reinterpret_cast<sockaddr*>(&address),
                &addressLength) == 0);
            commandPort_ = ntohs(address.sin_port);
            if (commandPort_ != 0xFFFFU)
            {
                dataSocket_ = OpenBoundSocket(
                    static_cast<std::uint16_t>(commandPort_ + 1U));
                if (dataSocket_ >= 0)
                {
                    return;
                }
            }
            ::close(commandSocket_);
            commandSocket_ = -1;
        }
        FAIL("could not reserve adjacent localhost command and data ports");
    }

    void SetError(std::string error)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (error_.empty())
        {
            error_ = std::move(error);
        }
    }

    void Run()
    {
        while (!stop_.load())
        {
            std::array<pollfd, 2> descriptors{{
                {commandSocket_, POLLIN, 0},
                {dataSocket_, POLLIN, 0},
            }};
            const int ready = ::poll(
                descriptors.data(), descriptors.size(), 20);
            if (ready < 0)
            {
                SetError("localhost emulator poll failed");
                return;
            }
            if ((descriptors[0].revents & POLLIN) != 0)
            {
                ReceiveCommand();
            }
            if ((descriptors[1].revents & POLLIN) != 0)
            {
                ReceiveRedirectAndSendData();
            }
        }
    }

    void ReceiveCommand()
    {
        std::array<std::byte, 9000> bytes{};
        sockaddr_in sender{};
        socklen_t senderLength = sizeof(sender);
        const ssize_t received = ::recvfrom(
            commandSocket_,
            bytes.data(),
            bytes.size(),
            0,
            reinterpret_cast<sockaddr*>(&sender),
            &senderLength);
        if (received <= 0
            || received % static_cast<ssize_t>(sizeof(std::uint32_t)) != 0)
        {
            SetError("localhost emulator received a malformed command");
            return;
        }

        std::vector<std::uint32_t> words;
        for (std::size_t offset = 0U;
             offset < static_cast<std::size_t>(received);
             offset += sizeof(std::uint32_t))
        {
            words.push_back(fidget::LoadLittleEndian32(bytes.data() + offset));
        }
        const auto reply = HandleCommand(words);
        if (reply.empty())
        {
            return;
        }
        const ssize_t sent = ::sendto(
            commandSocket_,
            reply.data(),
            reply.size(),
            0,
            reinterpret_cast<const sockaddr*>(&sender),
            senderLength);
        if (sent != static_cast<ssize_t>(reply.size()))
        {
            SetError("localhost emulator could not send a command reply");
        }
    }

    std::vector<std::byte> HandleCommand(
        const std::vector<std::uint32_t>& words)
    {
        if (words.size() < 3U
            || words.front() != fidget::MvlcCommandBufferStart
            || words.back() != fidget::MvlcCommandBufferEnd
            || (words[1] & 0xFFFF0000U)
                != fidget::MvlcReferenceWordCommand)
        {
            SetError("localhost emulator received an unknown command shape");
            return {};
        }

        const auto reference = static_cast<std::uint16_t>(words[1]);
        std::vector<std::uint16_t> reads;
        std::vector<std::pair<std::uint16_t, std::uint32_t>> writes;
        for (std::size_t index = 2U; index + 1U < words.size(); ++index)
        {
            const std::uint32_t command = words[index] & 0xFFFF0000U;
            if (command == fidget::MvlcReadLocalCommand)
            {
                reads.push_back(static_cast<std::uint16_t>(words[index]));
            }
            else if (command == fidget::MvlcWriteLocalCommand)
            {
                if (index + 1U >= words.size() - 1U)
                {
                    SetError("localhost emulator saw a truncated local write");
                    return {};
                }
                writes.push_back({
                    static_cast<std::uint16_t>(words[index]),
                    words[index + 1U],
                });
                ++index;
            }
        }

        if (!reads.empty())
        {
            std::vector<std::uint32_t> frame{
                (static_cast<std::uint32_t>(fidget::MvlcSuperFrameType)
                 << 24U)
                    | static_cast<std::uint32_t>(1U + reads.size() * 2U),
                fidget::MvlcReferenceWordCommand | reference,
            };
            const std::lock_guard<std::mutex> lock(mutex_);
            for (const auto address : reads)
            {
                frame.push_back(fidget::MvlcReadLocalCommand | address);
                frame.push_back(localRegisters_[address]);
            }
            return MakeCommandPacket({frame});
        }

        const auto stackStart = std::find_if(
            writes.begin(),
            writes.end(),
            [](const auto& write) {
                return write.second == fidget::MvlcStackStartCommand;
            });
        if (stackStart != writes.end())
        {
            CaptureUploadedVmeOperation(writes);
            return MakeCommandPacket({SuperFrame(reference)});
        }

        const auto execute = std::find_if(
            writes.begin(),
            writes.end(),
            [](const auto& write) {
                return write.first == fidget::MvlcStack0TriggerRegister
                    && write.second == fidget::MvlcImmediateTrigger;
            });
        if (execute != writes.end())
        {
            return ExecuteUploadedVmeOperation(reference);
        }

        {
            const std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& write : writes)
            {
                if (!firstStackWriteSeen_.load()
                    && write.first >= 0x2200U
                    && write.first <= 0x221CU)
                {
                    firstStackWriteSeen_.store(true);
                    firstStackWriteSawJournal_.store(
                        std::filesystem::exists(journalPath_));
                }
                localRegisters_[write.first] = write.second;
            }
        }
        return MakeCommandPacket({SuperFrame(reference)});
    }

    void CaptureUploadedVmeOperation(
        const std::vector<std::pair<std::uint16_t, std::uint32_t>>& writes)
    {
        std::vector<std::uint32_t> values;
        values.reserve(writes.size());
        for (const auto& write : writes)
        {
            values.push_back(write.second);
        }

        PendingVmeOperation next;
        const auto marker = std::find(
            values.begin(), values.end(), fidget::MvlcWriteMarkerCommand);
        if (marker == values.end() || marker + 1 == values.end())
        {
            SetError("localhost emulator could not find the stack reference");
            return;
        }
        next.stackReference = *(marker + 1);

        const auto read = std::find(
            values.begin(), values.end(), fidget::MvlcVmeReadA32D16Command);
        const auto write = std::find(
            values.begin(), values.end(), fidget::MvlcVmeWriteA32D16Command);
        if (read != values.end() && read + 1 != values.end())
        {
            next.kind = PendingVmeKind::Read;
            next.address = *(read + 1);
        }
        else if (write != values.end()
                 && write + 2 < values.end())
        {
            next.kind = PendingVmeKind::Write;
            next.address = *(write + 1);
            next.value = static_cast<std::uint16_t>(*(write + 2));
        }
        else
        {
            SetError("localhost emulator could not decode the VME operation");
            return;
        }

        const std::lock_guard<std::mutex> lock(mutex_);
        pendingVme_ = next;
    }

    std::vector<std::byte> ExecuteUploadedVmeOperation(
        const std::uint16_t reference)
    {
        PendingVmeOperation operation;
        std::uint16_t readValue = 0U;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            operation = pendingVme_;
            if (operation.kind == PendingVmeKind::Read)
            {
                readValue = vmeRegisters_[operation.address];
            }
            else if (operation.kind == PendingVmeKind::Write)
            {
                vmeRegisters_[operation.address] = operation.value;
            }
            pendingVme_ = {};
        }

        if (operation.kind == PendingVmeKind::Read)
        {
            return MakeCommandPacket({
                SuperFrame(reference),
                {
                    (static_cast<std::uint32_t>(fidget::MvlcStackFrameType)
                     << 24U)
                        | 2U,
                    operation.stackReference,
                    readValue,
                },
            });
        }
        if (operation.kind == PendingVmeKind::Write)
        {
            return MakeCommandPacket({
                SuperFrame(reference),
                {
                    (static_cast<std::uint32_t>(fidget::MvlcStackFrameType)
                     << 24U)
                        | 1U,
                    operation.stackReference,
                },
            });
        }
        SetError("localhost emulator executed without an uploaded stack");
        return {};
    }

    void ReceiveRedirectAndSendData()
    {
        std::array<std::byte, 64> redirect{};
        sockaddr_in receiver{};
        socklen_t receiverLength = sizeof(receiver);
        const ssize_t received = ::recvfrom(
            dataSocket_,
            redirect.data(),
            redirect.size(),
            0,
            reinterpret_cast<sockaddr*>(&receiver),
            &receiverLength);
        if (received != 2 * static_cast<ssize_t>(sizeof(std::uint32_t)))
        {
            SetError("localhost emulator received a malformed redirect");
            return;
        }
        for (std::uint16_t packet = 1U; packet <= 3U; ++packet)
        {
            const auto waveform = MakeWaveformPacket(
                packet, static_cast<int>(packet) * 10);
            const ssize_t sent = ::sendto(
                dataSocket_,
                waveform.data(),
                waveform.size(),
                0,
                reinterpret_cast<const sockaddr*>(&receiver),
                receiverLength);
            if (sent != static_cast<ssize_t>(waveform.size()))
            {
                SetError("localhost emulator could not send waveform data");
                return;
            }
        }
    }

    std::string journalPath_;
    int commandSocket_ = -1;
    int dataSocket_ = -1;
    std::uint16_t commandPort_ = 0U;
    mutable std::mutex mutex_;
    std::string error_;
    std::map<std::uint16_t, std::uint32_t> localRegisters_;
    std::map<std::uint32_t, std::uint16_t> vmeRegisters_;
    PendingVmeOperation pendingVme_;
    std::atomic<bool> firstStackWriteSeen_{false};
    std::atomic<bool> firstStackWriteSawJournal_{false};
    std::atomic<bool> stop_{false};
    std::thread thread_;
};

fidget::DiagnosticAcquisitionPreparationRequest MakeRequest(
    const LocalMvlcEmulator& emulator,
    const std::string& journalPath)
{
    fidget::DiagnosticAcquisitionPreparationRequest request;
    request.host = "127.0.0.1";
    request.commandPort = emulator.CommandPort();
    request.mvlcHardwareId = 0x5008U;
    request.mvlcFirmwareRevision = 0x0046U;
    request.targetBaseAddress = TargetBase;
    request.requestedChannel = 29U;
    request.configuredModuleBaseAddresses = {TargetBase};
    request.recoveryJournalPath = journalPath;
    request.ownershipTokenValue = OwnershipToken;
    return request;
}

} // namespace

TEST_CASE("localhost MVLC command and data pipes complete a safe lifecycle")
{
    using namespace fidget;

    JournalPath journal;
    LocalMvlcEmulator emulator(journal.Get());
    MvlcCommandTransport commandTransport;
    MvlcDataReceiver dataReceiver;
    const auto opened = commandTransport.Open(
        "127.0.0.1", emulator.CommandPort());
    INFO(opened.error);
    REQUIRE(opened.success);

    const auto request = MakeRequest(emulator, journal.Get());
    const std::atomic<bool> cancelled{false};
    auto prepared = PrepareDiagnosticAcquisition(
        commandTransport,
        request,
        cancelled,
        [](const std::string&) {
            return ScpCaptureGateResult{
                ScpCaptureGateStatus::Allowed,
                {},
            };
        });
    REQUIRE(prepared.acquisition.state
            == DiagnosticAcquisitionState::Starting);
    REQUIRE(std::filesystem::exists(journal.Get()));

    prepared = StartPreparedDiagnosticAcquisition(
        commandTransport,
        dataReceiver,
        std::move(prepared),
        request,
        cancelled);
    INFO(prepared.acquisition.message);
    REQUIRE(prepared.acquisition.state
            == DiagnosticAcquisitionState::Running);
    CHECK(emulator.FirstStackWriteSeen());
    CHECK(emulator.FirstStackWriteSawJournal());

    std::mutex publishedMutex;
    std::condition_variable publishedWakeup;
    DiagnosticStreamSnapshot published;
    AcquisitionReceiver receiver(
        dataReceiver, std::chrono::milliseconds(5));
    REQUIRE(receiver.Start(
        29U,
        [&](const DiagnosticStreamSnapshot& next) {
            {
                const std::lock_guard<std::mutex> lock(publishedMutex);
                published = next;
            }
            publishedWakeup.notify_all();
        }));
    {
        std::unique_lock<std::mutex> lock(publishedMutex);
        REQUIRE(publishedWakeup.wait_for(
            lock,
            std::chrono::seconds(2),
            [&] {
                return published.decoderStats.decodedWaveforms >= 3U;
            }));
    }
    CHECK(published.decoderStats.lostEthernetPackets == 0U);
    CHECK(published.decoderStats.malformedWords == 0U);
    CHECK(published.requestedTarget.channelObserved);

    std::uint16_t heartbeatReference = prepared.nextSuperReference;
    const auto fingerprint = VerifyDiagnosticOwnershipFingerprint(
        commandTransport,
        prepared,
        heartbeatReference,
        cancelled);
    INFO(fingerprint.message);
    CHECK(fingerprint.outcome == DiagnosticFingerprintOutcome::Verified);

    receiver.StopAndJoin();
    prepared.nextSuperReference = heartbeatReference;
    prepared = StopDiagnosticAcquisition(
        commandTransport,
        dataReceiver,
        std::move(prepared),
        request,
        cancelled);
    INFO(prepared.acquisition.message);
    CHECK(prepared.acquisition.state
          == DiagnosticAcquisitionState::Stopped);
    CHECK(prepared.acquisition.recoveryJournalRemoved);
    CHECK_FALSE(std::filesystem::exists(journal.Get()));
    CHECK(emulator.VmeRegister(
              TargetBase + DiagnosticAcquisitionControlRegister)
          == 0U);
    CHECK(emulator.LocalRegister(DiagnosticDaqModeRegister) == 0U);
    CHECK(emulator.LocalRegister(
              prepared.readoutPlan.stackTriggerRegister)
          == 0U);
    CHECK(emulator.LocalRegister(
              prepared.readoutPlan.stackOffsetRegister)
          == 0U);
    CHECK(emulator.LocalRegister(
              prepared.recoveryRecord.ownershipTokenRegister)
          == 0U);
    CHECK(emulator.Error().empty());
}
