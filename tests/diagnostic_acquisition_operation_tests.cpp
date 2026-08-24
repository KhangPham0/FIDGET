#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/RecoveryJournal.h"
#include "hardware/DiagnosticAcquisitionOperation.h"
#include "hardware/VmeTransaction.h"
#include "vme_test_support.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t TargetBase = 0x11000000U;
constexpr std::uint32_t OtherBase = 0x22000000U;
constexpr std::uint32_t OwnershipToken = 0xA55A1234U;

class FakeDataReceiver final : public fidget::IDataReceiver
{
public:
    [[nodiscard]] fidget::TransportOperationResult Open(
        const std::string& host,
        const std::uint16_t port) override
    {
        host_ = host;
        port_ = port;
        open_ = true;
        return {true, {}};
    }

    [[nodiscard]] fidget::TransportOperationResult Send(
        const std::byte* data,
        const std::size_t size) override
    {
        if (!open_)
        {
            return {false, "fake data send: receiver is closed"};
        }
        sent_.emplace_back(data, data + size);
        return {true, {}};
    }

    [[nodiscard]] fidget::TransportReceiveResult Receive(
        std::byte*, std::size_t, int) override
    {
        return {
            fidget::TransportReceiveStatus::Timeout,
            0U,
            "data receive: timed out",
        };
    }

    void Close() noexcept override
    {
        open_ = false;
    }

    [[nodiscard]] bool IsOpen() const noexcept
    {
        return open_;
    }

    [[nodiscard]] const std::string& Host() const noexcept
    {
        return host_;
    }

    [[nodiscard]] std::uint16_t Port() const noexcept
    {
        return port_;
    }

    [[nodiscard]] const std::vector<std::vector<std::byte>>& Sent() const
        noexcept
    {
        return sent_;
    }

private:
    bool open_ = false;
    std::string host_;
    std::uint16_t port_ = 0U;
    std::vector<std::vector<std::byte>> sent_;
};

class JournalPath
{
public:
    JournalPath()
    {
        const auto unique = std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count();
        path_ = (std::filesystem::temp_directory_path()
                 / ("fidget-acquisition-" + std::to_string(unique)
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

fidget::DiagnosticAcquisitionPreparationRequest MakeRequest(
    const std::string& journalPath)
{
    fidget::DiagnosticAcquisitionPreparationRequest request;
    request.host = "mvlc-test";
    request.commandPort = 32768U;
    request.mvlcHardwareId = 0x5008U;
    request.mvlcFirmwareRevision = 0x0046U;
    request.targetBaseAddress = TargetBase;
    request.requestedChannel = 29U;
    request.configuredModuleBaseAddresses = {TargetBase, OtherBase};
    request.recoveryJournalPath = journalPath;
    request.ownershipTokenValue = OwnershipToken;
    return request;
}

fidget::ScpCaptureOwnershipGate AllowOwnership()
{
    return [](const std::string&) {
        return fidget::ScpCaptureGateResult{
            fidget::ScpCaptureGateStatus::Allowed,
            {},
        };
    };
}

void QueueTargetValidation(
    fidget::test::FakeCommandTransport& transport,
    fidget::test::TransactionReferences& references,
    const std::uint16_t hardwareId = 0x5007U,
    const std::uint16_t irqLevel = 3U)
{
    fidget::test::QueueRead(
        transport,
        references,
        TargetBase + fidget::DiagnosticHardwareIdRegister,
        hardwareId);
    if (hardwareId != 0x5007U && hardwareId != 0x500CU)
    {
        return;
    }
    fidget::test::QueueRead(
        transport,
        references,
        TargetBase + fidget::DiagnosticOutputFormatRegister,
        0x0018U);
    fidget::test::QueueRead(
        transport,
        references,
        TargetBase + fidget::DiagnosticIrqLevelRegister,
        irqLevel);
}

void QueueIsolationCapture(
    fidget::test::FakeCommandTransport& transport,
    fidget::test::TransactionReferences& references,
    const std::uint16_t irqLevel,
    const std::uint16_t acquisitionState)
{
    fidget::test::QueueRead(
        transport,
        references,
        OtherBase + fidget::DiagnosticHardwareIdRegister,
        0x5007U);
    fidget::test::QueueRead(
        transport,
        references,
        OtherBase + fidget::DiagnosticIrqLevelRegister,
        irqLevel);
    fidget::test::QueueRead(
        transport,
        references,
        OtherBase + fidget::DiagnosticAcquisitionControlRegister,
        acquisitionState);
}

void QueueIsolationWrites(
    fidget::test::FakeCommandTransport& transport,
    fidget::test::TransactionReferences& references,
    const bool stopRequired)
{
    if (stopRequired)
    {
        fidget::test::QueueWrite(
            transport,
            references,
            OtherBase + fidget::DiagnosticAcquisitionControlRegister,
            0U);
    }
    fidget::test::QueueRead(
        transport,
        references,
        OtherBase + fidget::DiagnosticAcquisitionControlRegister,
        0U);
    fidget::test::QueueWrite(
        transport,
        references,
        OtherBase + fidget::DiagnosticFifoResetRegister,
        1U);
    fidget::test::QueueWrite(
        transport,
        references,
        OtherBase + fidget::DiagnosticReadoutResetRegister,
        1U);
}

void QueueLocalWrite(
    fidget::test::FakeCommandTransport& transport,
    fidget::test::TransactionReferences& references,
    const std::vector<fidget::MvlcLocalRegisterWrite>& writes)
{
    const auto request = fidget::BuildMvlcLocalRegisterWriteRequest(
        references.super, writes.data(), writes.size());
    REQUIRE(request.success);
    transport.QueueExchange({
        fidget::test::EncodeWords(request.words),
        {fidget::test::FakeReceiveAction::Datagram(
            fidget::test::MakeCommandPacket({
                fidget::test::MakeSuperFrame(references.super),
            }))},
    });
    ++references.super;
}

void QueueLocalRead(
    fidget::test::FakeCommandTransport& transport,
    fidget::test::TransactionReferences& references,
    const std::uint16_t address,
    const std::uint32_t value)
{
    const auto request = fidget::BuildMvlcLocalRegisterBatchReadRequest(
        references.super, &address, 1U);
    REQUIRE(request.success);
    const std::vector<std::uint32_t> frame{
        (static_cast<std::uint32_t>(fidget::MvlcSuperFrameType) << 24U)
            | 3U,
        fidget::MvlcReferenceWordCommand | references.super,
        fidget::MvlcReadLocalCommand | address,
        value,
    };
    transport.QueueExchange({
        fidget::test::EncodeWords(request.words),
        {fidget::test::FakeReceiveAction::Datagram(
            fidget::test::MakeCommandPacket({frame}))},
    });
    ++references.super;
}

std::vector<std::uint16_t> FingerprintAddresses(
    const fidget::DiagnosticAcquisitionPreparationResult& prepared)
{
    std::vector<std::uint16_t> addresses{
        fidget::DiagnosticDaqModeRegister,
        prepared.readoutPlan.stackTriggerRegister,
        prepared.readoutPlan.stackOffsetRegister,
    };
    for (const auto& write : prepared.readoutPlan.stackUploadWrites)
    {
        addresses.push_back(write.address);
    }
    addresses.push_back(prepared.recoveryRecord.ownershipTokenRegister);
    return addresses;
}

std::vector<std::uint32_t> FingerprintValues(
    const fidget::DiagnosticAcquisitionPreparationResult& prepared)
{
    std::vector<std::uint32_t> values{
        fidget::DiagnosticDaqEnableValue,
        prepared.readoutPlan.triggerValue,
        prepared.readoutPlan.stackMemoryOffset,
    };
    for (const auto& write : prepared.readoutPlan.stackUploadWrites)
    {
        values.push_back(write.value);
    }
    values.push_back(prepared.recoveryRecord.ownershipTokenValue);
    return values;
}

void QueueFingerprintRead(
    fidget::test::FakeCommandTransport& transport,
    const std::uint16_t reference,
    const std::vector<std::uint16_t>& addresses,
    const std::vector<std::uint32_t>& values)
{
    REQUIRE(addresses.size() == values.size());
    const auto request = fidget::BuildMvlcLocalRegisterBatchReadRequest(
        reference, addresses.data(), addresses.size());
    REQUIRE(request.success);
    std::vector<std::uint32_t> frame{
        (static_cast<std::uint32_t>(fidget::MvlcSuperFrameType) << 24U)
            | static_cast<std::uint32_t>(1U + addresses.size() * 2U),
        fidget::MvlcReferenceWordCommand | reference,
    };
    for (std::size_t index = 0U; index < addresses.size(); ++index)
    {
        frame.push_back(fidget::MvlcReadLocalCommand | addresses[index]);
        frame.push_back(values[index]);
    }
    transport.QueueExchange({
        fidget::test::EncodeWords(request.words),
        {fidget::test::FakeReceiveAction::Datagram(
            fidget::test::MakeCommandPacket({frame}))},
    });
}

} // namespace

TEST_CASE("an active IRQ-zero non-target is isolated and journaled")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueTargetValidation(transport, references);
    QueueIsolationCapture(transport, references, 0U, 1U);
    QueueIsolationWrites(transport, references, true);

    const std::atomic<bool> cancelled{false};
    std::vector<std::string> gateNames;
    bool journalReadyBeforeQuiescence = false;
    std::size_t requestsBeforeQuiescence = 0U;
    const ScpCaptureOwnershipGate ownershipGate =
        [&](const std::string& operationName) {
            gateNames.push_back(operationName);
            if (gateNames.size() == 2U)
            {
                const auto loaded = LoadTunerRecoveryJournal(journal.Get());
                journalReadyBeforeQuiescence = loaded.success
                    && loaded.record.has_value()
                    && loaded.record->phase == TunerRecoveryPhase::Prepared
                    && loaded.record->isolatedModuleBaseAddresses
                        == std::vector<std::uint32_t>{OtherBase};
                requestsBeforeQuiescence =
                    transport.SentRequests().size();
            }
            return ScpCaptureGateResult{
                ScpCaptureGateStatus::Allowed,
                {},
            };
        };
    const auto prepared = PrepareDiagnosticAcquisition(
        transport,
        MakeRequest(journal.Get()),
        cancelled,
        ownershipGate);

    const auto& result = prepared.acquisition;
    CHECK(result.state == DiagnosticAcquisitionState::Starting);
    CHECK(result.requestedChannel == 29U);
    CHECK(result.activeNonTargetModulesFound == 1U);
    CHECK(result.nonTargetModulesQuiesced == 1U);
    CHECK(result.recoveryJournalPrepared);
    REQUIRE(result.moduleIsolation.size() == 1U);
    const auto& isolated = result.moduleIsolation.front();
    CHECK(isolated.validated);
    CHECK(isolated.irqLevel == 0U);
    CHECK_FALSE(isolated.sharesTargetIrq);
    CHECK(isolated.stopRequired);
    CHECK(isolated.stopSent);
    CHECK(isolated.stopVerified);
    CHECK(isolated.fifoResetSent);
    CHECK(isolated.readoutResetSent);
    CHECK(gateNames == std::vector<std::string>{
        "direct diagnostic acquisition",
        "non-target MDPP quiescence",
    });
    CHECK(journalReadyBeforeQuiescence);
    CHECK(requestsBeforeQuiescence == 12U);

    const auto loaded = LoadTunerRecoveryJournal(journal.Get());
    REQUIRE(loaded.success);
    REQUIRE(loaded.record.has_value());
    const auto& record = *loaded.record;
    CHECK(record.phase == TunerRecoveryPhase::Prepared);
    CHECK(record.host == "mvlc-test");
    CHECK(record.commandPort == 32768U);
    CHECK(record.mvlcHardwareId == 0x5008U);
    CHECK(record.mvlcFirmwareRevision == 0x0046U);
    CHECK(record.mdppBaseAddress == TargetBase);
    CHECK(record.mdppHardwareId == 0x5007U);
    CHECK(record.mdppIrqLevel == 3U);
    CHECK(record.mdppOutputFormat == 0x0018U);
    CHECK(record.stackTriggerRegister == 0x1104U);
    CHECK(record.stackTriggerValue == 0x0042U);
    CHECK(record.stackOffsetRegister == 0x1204U);
    CHECK(record.stackOffsetValue == 0x0200U);
    CHECK(record.ownershipTokenRegister == 0x221CU);
    CHECK(record.ownershipTokenValue == OwnershipToken);
    CHECK(record.isolatedModuleBaseAddresses
          == std::vector<std::uint32_t>{OtherBase});

    const auto operations = DecodeWireOperations(transport);
    REQUIRE(operations.size() == 10U);
    CHECK(operations[6].write);
    CHECK(operations[6].address
          == OtherBase + DiagnosticAcquisitionControlRegister);
    CHECK(operations[6].value == 0U);
    CHECK(operations[8].write);
    CHECK(operations[8].address == OtherBase + DiagnosticFifoResetRegister);
    CHECK(operations[9].write);
    CHECK(operations[9].address
          == OtherBase + DiagnosticReadoutResetRegister);
}

TEST_CASE("an already stopped non-target is reset without a stop write")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueTargetValidation(transport, references);
    QueueIsolationCapture(transport, references, 3U, 0U);
    QueueIsolationWrites(transport, references, false);

    const std::atomic<bool> cancelled{false};
    const auto prepared = PrepareDiagnosticAcquisition(
        transport,
        MakeRequest(journal.Get()),
        cancelled,
        AllowOwnership());

    REQUIRE(prepared.acquisition.recoveryJournalPrepared);
    REQUIRE(prepared.acquisition.moduleIsolation.size() == 1U);
    const auto& isolated = prepared.acquisition.moduleIsolation.front();
    CHECK(isolated.sharesTargetIrq);
    CHECK_FALSE(isolated.stopRequired);
    CHECK_FALSE(isolated.stopSent);
    CHECK(isolated.stopVerified);

    const auto operations = DecodeWireOperations(transport);
    REQUIRE(operations.size() == 9U);
    std::size_t acquisitionWrites = 0U;
    for (const auto& operation : operations)
    {
        if (operation.write
            && operation.address
                == OtherBase + DiagnosticAcquisitionControlRegister)
        {
            ++acquisitionWrites;
        }
    }
    CHECK(acquisitionWrites == 0U);
}

TEST_CASE("an unsupported target is rejected before isolation or journaling")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueTargetValidation(transport, references, 0x1234U);

    const std::atomic<bool> cancelled{false};
    const auto prepared = PrepareDiagnosticAcquisition(
        transport,
        MakeRequest(journal.Get()),
        cancelled,
        AllowOwnership());

    CHECK(prepared.acquisition.state == DiagnosticAcquisitionState::Failed);
    CHECK_FALSE(prepared.acquisition.recoveryJournalPrepared);
    CHECK(prepared.acquisition.message
          == "The target does not identify as a supported MDPP-32 module.");
    CHECK(transport.SentRequests().size() == 2U);
    CHECK_FALSE(std::filesystem::exists(journal.Get()));
}

TEST_CASE("read-only MDPP-32 v2 recognition does not authorize acquisition writes")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueTargetValidation(
        transport, references, Mdpp32AlternateHardwareId);

    const std::atomic<bool> cancelled{false};
    const auto prepared = PrepareDiagnosticAcquisition(
        transport,
        MakeRequest(journal.Get()),
        cancelled,
        AllowOwnership());

    INFO(prepared.acquisition.message);
    CHECK(prepared.acquisition.state == DiagnosticAcquisitionState::Failed);
    CHECK(prepared.acquisition.message.find(
              "await recorded hardware acceptance")
          != std::string::npos);
    CHECK_FALSE(prepared.acquisition.recoveryJournalPrepared);
    const auto operations = DecodeWireOperations(transport);
    REQUIRE(operations.size() == 1U);
    CHECK_FALSE(operations.front().write);
    CHECK_FALSE(std::filesystem::exists(journal.Get()));
}

TEST_CASE("acquisition cannot begin when recovery journaling is unavailable")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueTargetValidation(transport, references);
    QueueIsolationCapture(transport, references, 0U, 0U);

    auto request = MakeRequest(journal.Get());
    std::size_t saveCalls = 0U;
    TunerRecoveryRecord attemptedRecord;
    const DiagnosticAcquisitionJournalSaver rejectSave =
        [&](const TunerRecoveryRecord& record, const std::string& path) {
            ++saveCalls;
            attemptedRecord = record;
            CHECK(path == journal.Get());
            return TunerRecoverySaveResult{
                false,
                "injected recovery-journal save failure",
            };
        };
    const std::atomic<bool> cancelled{false};
    const auto prepared = PrepareDiagnosticAcquisition(
        transport,
        request,
        cancelled,
        AllowOwnership(),
        rejectSave);

    CHECK(prepared.acquisition.state == DiagnosticAcquisitionState::Failed);
    CHECK_FALSE(prepared.acquisition.recoveryJournalPrepared);
    CHECK(saveCalls == 1U);
    CHECK(attemptedRecord.phase == TunerRecoveryPhase::Prepared);
    CHECK(attemptedRecord.mdppBaseAddress == TargetBase);
    CHECK(attemptedRecord.isolatedModuleBaseAddresses
          == std::vector<std::uint32_t>{OtherBase});
    CHECK(prepared.acquisition.message.find(
              "Could not prepare crash recovery before changing any "
              "configured module state:")
          == 0U);
    CHECK(prepared.acquisition.message.find(
              "injected recovery-journal save failure")
          != std::string::npos);
    REQUIRE(prepared.acquisition.moduleIsolation.size() == 1U);
    CHECK(prepared.acquisition.moduleIsolation.front().validated);
    CHECK_FALSE(
        prepared.acquisition.moduleIsolation.front().quiescenceAttempted);

    const auto operations = DecodeWireOperations(transport);
    REQUIRE(operations.size() == 6U);
    for (const auto& operation : operations)
    {
        CHECK_FALSE(operation.write);
    }
    CHECK(transport.SentRequests().size() == 12U);
    CHECK_FALSE(std::filesystem::exists(journal.Get()));
}

TEST_CASE("the fresh ownership gate runs after journaling and before quiescence")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueTargetValidation(transport, references);
    QueueIsolationCapture(transport, references, 0U, 1U);

    std::size_t gateCalls = 0U;
    const ScpCaptureOwnershipGate gate =
        [&gateCalls](const std::string&) {
            ++gateCalls;
            return gateCalls == 2U
                ? ScpCaptureGateResult{
                    ScpCaptureGateStatus::CommunicationUnavailable,
                    "The fresh DAQ-idle check was unavailable.",
                }
                : ScpCaptureGateResult{
                    ScpCaptureGateStatus::Allowed,
                    {},
                };
        };
    const std::atomic<bool> cancelled{false};
    const auto prepared = PrepareDiagnosticAcquisition(
        transport,
        MakeRequest(journal.Get()),
        cancelled,
        gate);

    CHECK(prepared.acquisition.state
          == DiagnosticAcquisitionState::Failed);
    CHECK(prepared.acquisition.recoveryJournalPrepared);
    CHECK(prepared.acquisition.orphanRecoveryRequired);
    CHECK(prepared.acquisition.communicationUncertain);
    CHECK(prepared.acquisition.commandPathFailures == 1U);
    CHECK(prepared.acquisition.message.find(
              "The fresh DAQ-idle check was unavailable.")
          != std::string::npos);
    CHECK(prepared.acquisition.message.find(
              "The Prepared recovery journal was retained.")
          != std::string::npos);
    REQUIRE(prepared.acquisition.moduleIsolation.size() == 1U);
    CHECK_FALSE(
        prepared.acquisition.moduleIsolation.front().quiescenceAttempted);
    CHECK(gateCalls == 2U);

    const auto loaded = LoadTunerRecoveryJournal(journal.Get());
    REQUIRE(loaded.success);
    REQUIRE(loaded.record.has_value());
    CHECK(loaded.record->phase == TunerRecoveryPhase::Prepared);
    CHECK(loaded.record->isolatedModuleBaseAddresses
          == std::vector<std::uint32_t>{OtherBase});

    const auto operations = DecodeWireOperations(transport);
    REQUIRE(operations.size() == 6U);
    for (const auto& operation : operations)
    {
        CHECK_FALSE(operation.write);
    }
}

TEST_CASE("the prepared journal survives every interrupted isolation write")
{
    using namespace fidget;
    using namespace fidget::test;

    struct Interruption
    {
        const char* name = nullptr;
        std::size_t executeSend = 0U;
        std::size_t expectedOperationCount = 0U;
    };
    const std::vector<Interruption> interruptions{
        {"module stop", 14U, 7U},
        {"FIFO reset", 18U, 9U},
        {"readout reset", 20U, 10U},
    };
    const std::vector<WireOperation> expectedOperations{
        {false, TargetBase + DiagnosticHardwareIdRegister, 0U},
        {false, TargetBase + DiagnosticOutputFormatRegister, 0U},
        {false, TargetBase + DiagnosticIrqLevelRegister, 0U},
        {false, OtherBase + DiagnosticHardwareIdRegister, 0U},
        {false, OtherBase + DiagnosticIrqLevelRegister, 0U},
        {false, OtherBase + DiagnosticAcquisitionControlRegister, 0U},
        {true, OtherBase + DiagnosticAcquisitionControlRegister, 0U},
        {false, OtherBase + DiagnosticAcquisitionControlRegister, 0U},
        {true, OtherBase + DiagnosticFifoResetRegister, 1U},
        {true, OtherBase + DiagnosticReadoutResetRegister, 1U},
    };

    for (const auto& interruption : interruptions)
    {
        INFO("interrupt " << interruption.name
                           << " after send "
                           << interruption.executeSend);
        JournalPath journal;
        FakeCommandTransport transport;
        Open(transport);
        TransactionReferences references{0x5000U, 0x9E000001U};
        QueueTargetValidation(transport, references);
        QueueIsolationCapture(transport, references, 0U, 1U);
        QueueIsolationWrites(transport, references, true);

        std::atomic<bool> cancelled{false};
        std::size_t sends = 0U;
        bool journalReadyBeforeWrite = false;
        transport.SetSendHook([&](const std::vector<std::byte>&) {
            ++sends;
            if (sends + 1U == interruption.executeSend)
            {
                const auto loaded =
                    LoadTunerRecoveryJournal(journal.Get());
                journalReadyBeforeWrite = loaded.success
                    && loaded.record.has_value()
                    && loaded.record->phase == TunerRecoveryPhase::Prepared
                    && loaded.record->isolatedModuleBaseAddresses
                        == std::vector<std::uint32_t>{OtherBase};
            }
            if (sends == interruption.executeSend)
            {
                cancelled.store(true);
            }
        });

        const auto prepared = PrepareDiagnosticAcquisition(
            transport,
            MakeRequest(journal.Get()),
            cancelled,
            AllowOwnership());

        CHECK(prepared.acquisition.state
              == DiagnosticAcquisitionState::Failed);
        CHECK(prepared.acquisition.recoveryJournalPrepared);
        CHECK(prepared.acquisition.orphanRecoveryRequired);
        CHECK(prepared.acquisition.message.find(
                  "The Prepared recovery journal was retained.")
              != std::string::npos);
        CHECK(journalReadyBeforeWrite);
        CHECK(sends == interruption.executeSend);

        const auto loaded = LoadTunerRecoveryJournal(journal.Get());
        REQUIRE(loaded.success);
        REQUIRE(loaded.record.has_value());
        CHECK(loaded.record->phase == TunerRecoveryPhase::Prepared);
        CHECK(loaded.record->isolatedModuleBaseAddresses
              == std::vector<std::uint32_t>{OtherBase});

        const auto operations = DecodeWireOperations(transport);
        REQUIRE(operations.size() == interruption.expectedOperationCount);
        REQUIRE(operations.size() <= expectedOperations.size());
        for (std::size_t index = 0U;
             index < operations.size();
             ++index)
        {
            CHECK(operations[index].write
                  == expectedOperations[index].write);
            CHECK(operations[index].address
                  == expectedOperations[index].address);
            CHECK(operations[index].value
                  == expectedOperations[index].value);
        }
        CHECK(transport.SentRequests().size()
              == interruption.executeSend);
    }
}

TEST_CASE("acquisition refuses an empty project recovery path")
{
    using namespace fidget;
    using namespace fidget::test;

    FakeCommandTransport transport;
    Open(transport);
    const std::atomic<bool> cancelled{false};
    const auto prepared = PrepareDiagnosticAcquisition(
        transport, MakeRequest(""), cancelled, AllowOwnership());

    CHECK(prepared.acquisition.state == DiagnosticAcquisitionState::Failed);
    CHECK(prepared.acquisition.message
          == "Direct acquisition requires a recovery-journal path.");
    CHECK(transport.SentRequests().empty());
}

TEST_CASE("a prepared acquisition installs the stack starts and journals active")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences queued{0x5000U, 0x9E000001U};
    QueueTargetValidation(transport, queued);

    auto request = MakeRequest(journal.Get());
    request.configuredModuleBaseAddresses = {TargetBase};
    const std::atomic<bool> cancelled{false};
    auto prepared = PrepareDiagnosticAcquisition(
        transport, request, cancelled, AllowOwnership());
    REQUIRE(prepared.acquisition.recoveryJournalPrepared);
    CHECK(prepared.nextSuperReference == queued.super);
    CHECK(prepared.nextStackReference == queued.stack);

    std::vector<MvlcLocalRegisterWrite> stackWrites =
        prepared.readoutPlan.stackUploadWrites;
    stackWrites.push_back({0x221CU, OwnershipToken});
    QueueLocalWrite(transport, queued, stackWrites);
    QueueLocalWrite(
        transport,
        queued,
        {
            {0x1204U, 0x0200U},
            {0x1104U, 0U},
        });
    QueueLocalWrite(
        transport,
        queued,
        {{0x1104U, prepared.readoutPlan.triggerValue}});
    QueueWrite(
        transport,
        queued,
        TargetBase + DiagnosticAcquisitionControlRegister,
        0U);
    QueueWrite(
        transport,
        queued,
        TargetBase + DiagnosticFifoResetRegister,
        1U);
    QueueWrite(
        transport,
        queued,
        TargetBase + DiagnosticReadoutResetRegister,
        1U);
    QueueWrite(
        transport,
        queued,
        TargetBase + DiagnosticAcquisitionControlRegister,
        1U);
    QueueLocalWrite(
        transport,
        queued,
        {{DiagnosticDaqModeRegister, DiagnosticDaqEnableValue}});
    QueueLocalRead(
        transport,
        queued,
        DiagnosticDaqModeRegister,
        DiagnosticDaqEnableValue);

    FakeDataReceiver dataReceiver;
    prepared = StartPreparedDiagnosticAcquisition(
        transport,
        dataReceiver,
        std::move(prepared),
        request,
        cancelled);

    CHECK(prepared.acquisition.state == DiagnosticAcquisitionState::Running);
    CHECK(prepared.acquisition.dataPort == 32769U);
    CHECK(prepared.acquisition.recoveryJournalActive);
    CHECK(dataReceiver.IsOpen());
    CHECK(dataReceiver.Host() == "mvlc-test");
    CHECK(dataReceiver.Port() == 32769U);
    REQUIRE(dataReceiver.Sent().size() == 1U);
    const std::array<std::uint32_t, 2> redirectWords{
        MvlcCommandBufferStart,
        MvlcCommandBufferEnd,
    };
    CHECK(dataReceiver.Sent().front()
          == EncodeMvlcWordsLittleEndian(
              redirectWords.data(), redirectWords.size()));
    CHECK(prepared.nextSuperReference == queued.super);
    CHECK(prepared.nextStackReference == queued.stack);

    const auto loaded = LoadTunerRecoveryJournal(journal.Get());
    REQUIRE(loaded.success);
    REQUIRE(loaded.record.has_value());
    CHECK(loaded.record->phase == TunerRecoveryPhase::Active);

    const auto operations = DecodeWireOperations(transport);
    // The generic wire decoder also recognizes the VME-write opcode stored
    // inside the uploaded readout stack. The four final operations are the
    // actual MDPP stop, resets, and start transaction.
    REQUIRE(operations.size() == 8U);
    CHECK(operations[4].write);
    CHECK(operations[4].address
          == TargetBase + DiagnosticAcquisitionControlRegister);
    CHECK(operations[4].value == 0U);
    CHECK(operations[7].write);
    CHECK(operations[7].address
          == TargetBase + DiagnosticAcquisitionControlRegister);
    CHECK(operations[7].value == 1U);
}

TEST_CASE("command port 65535 refuses acquisition before opening data")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueTargetValidation(transport, references);

    auto request = MakeRequest(journal.Get());
    request.commandPort = 0xFFFFU;
    request.configuredModuleBaseAddresses = {TargetBase};
    const std::atomic<bool> cancelled{false};
    auto prepared = PrepareDiagnosticAcquisition(
        transport, request, cancelled, AllowOwnership());
    REQUIRE(prepared.acquisition.recoveryJournalPrepared);

    FakeDataReceiver dataReceiver;
    prepared = StartPreparedDiagnosticAcquisition(
        transport,
        dataReceiver,
        std::move(prepared),
        request,
        cancelled);

    CHECK(prepared.acquisition.state == DiagnosticAcquisitionState::Failed);
    CHECK(prepared.acquisition.message
          == "The MVLC command port has no adjacent data port.");
    CHECK_FALSE(dataReceiver.IsOpen());
    CHECK(dataReceiver.Sent().empty());
}

TEST_CASE("the complete ownership fingerprint is read in one transaction")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueTargetValidation(transport, references);
    auto request = MakeRequest(journal.Get());
    request.configuredModuleBaseAddresses = {TargetBase};
    const std::atomic<bool> cancelled{false};
    const auto prepared = PrepareDiagnosticAcquisition(
        transport, request, cancelled, AllowOwnership());
    REQUIRE(prepared.acquisition.recoveryJournalPrepared);

    const auto addresses = FingerprintAddresses(prepared);
    const auto values = FingerprintValues(prepared);
    REQUIRE(addresses.size() == 11U);
    constexpr std::uint16_t Reference = 0x6200U;
    QueueFingerprintRead(transport, Reference, addresses, values);
    std::uint16_t nextReference = Reference;
    const auto fingerprint = VerifyDiagnosticOwnershipFingerprint(
        transport, prepared, nextReference, cancelled);

    CHECK(fingerprint.outcome == DiagnosticFingerprintOutcome::Verified);
    CHECK(fingerprint.daqMode == DiagnosticDaqEnableValue);
    CHECK(fingerprint.message
          == "The complete unique tuner fingerprint matches.");
    CHECK(nextReference == Reference + 1U);

    const auto requests = transport.SentRequests();
    REQUIRE_FALSE(requests.empty());
    const auto words = DecodeWords(requests.back());
    REQUIRE(words.size() == 14U);
    CHECK(words.front() == MvlcCommandBufferStart);
    CHECK(words[1] == (MvlcReferenceWordCommand | Reference));
    for (std::size_t index = 0U; index < addresses.size(); ++index)
    {
        CHECK(words[index + 2U]
              == (MvlcReadLocalCommand | addresses[index]));
    }
    CHECK(words.back() == MvlcCommandBufferEnd);
}

TEST_CASE("six missing fingerprint replies become communication uncertainty")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueTargetValidation(transport, references);
    auto request = MakeRequest(journal.Get());
    request.configuredModuleBaseAddresses = {TargetBase};
    const std::atomic<bool> cancelled{false};
    const auto prepared = PrepareDiagnosticAcquisition(
        transport, request, cancelled, AllowOwnership());
    REQUIRE(prepared.acquisition.recoveryJournalPrepared);

    const auto addresses = FingerprintAddresses(prepared);
    const auto batch = BuildMvlcLocalRegisterBatchReadRequest(
        0x6300U, addresses.data(), addresses.size());
    REQUIRE(batch.success);
    for (int attempt = 0; attempt < MvlcFingerprintReadAttemptCount;
         ++attempt)
    {
        transport.QueueExchange({
            EncodeWords(batch.words),
            {FakeReceiveAction::Timeout()},
        });
    }

    const auto sendsBefore = transport.SentRequests().size();
    std::uint16_t nextReference = 0x6300U;
    const auto fingerprint = VerifyDiagnosticOwnershipFingerprint(
        transport, prepared, nextReference, cancelled);

    CHECK(fingerprint.outcome
          == DiagnosticFingerprintOutcome::CommunicationUnavailable);
    CHECK(fingerprint.message
          == "Could not read the complete tuner fingerprint in one MVLC "
             "transaction: No matching MVLC response after six "
             "register-batch attempts");
    CHECK(transport.SentRequests().size() - sendsBefore == 6U);
    CHECK(nextReference == 0x6301U);
}

TEST_CASE("a changed token is classified as a foreign fingerprint")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueTargetValidation(transport, references);
    auto request = MakeRequest(journal.Get());
    request.configuredModuleBaseAddresses = {TargetBase};
    const std::atomic<bool> cancelled{false};
    const auto prepared = PrepareDiagnosticAcquisition(
        transport, request, cancelled, AllowOwnership());
    REQUIRE(prepared.acquisition.recoveryJournalPrepared);

    const auto addresses = FingerprintAddresses(prepared);
    auto values = FingerprintValues(prepared);
    values.back() = 0x11112222U;
    QueueFingerprintRead(transport, 0x6400U, addresses, values);
    std::uint16_t nextReference = 0x6400U;
    const auto fingerprint = VerifyDiagnosticOwnershipFingerprint(
        transport, prepared, nextReference, cancelled);

    CHECK(fingerprint.outcome
          == DiagnosticFingerprintOutcome::ForeignFingerprint);
    CHECK(fingerprint.message
          == "Unique tuner ownership token changed at 0x0000221C: expected "
             "0xA55A1234, read 0x11112222.");
}

TEST_CASE("normal stop verifies zeros before removing the recovery journal")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences queued{0x5000U, 0x9E000001U};
    QueueTargetValidation(transport, queued);
    auto request = MakeRequest(journal.Get());
    request.configuredModuleBaseAddresses = {TargetBase};
    const std::atomic<bool> cancelled{false};
    auto prepared = PrepareDiagnosticAcquisition(
        transport, request, cancelled, AllowOwnership());
    REQUIRE(prepared.acquisition.recoveryJournalPrepared);
    prepared.acquisition.state = DiagnosticAcquisitionState::Running;
    prepared.acquisition.recoveryJournalActive = true;
    prepared.recoveryRecord.phase = TunerRecoveryPhase::Active;
    REQUIRE(SaveTunerRecoveryJournal(
        prepared.recoveryRecord, journal.Get()).success);

    const auto addresses = FingerprintAddresses(prepared);
    const auto values = FingerprintValues(prepared);
    QueueFingerprintRead(
        transport, prepared.nextSuperReference, addresses, values);
    ++queued.super;
    CHECK(prepared.nextSuperReference == queued.super - 1U);
    QueueWrite(
        transport,
        queued,
        TargetBase + DiagnosticAcquisitionControlRegister,
        0U);
    QueueRead(
        transport,
        queued,
        TargetBase + DiagnosticAcquisitionControlRegister,
        0U);
    QueueLocalWrite(
        transport,
        queued,
        {
            {DiagnosticDaqModeRegister, 0U},
            {0x1104U, 0U},
            {0x1204U, 0U},
            {0x221CU, 0U},
        });
    QueueLocalRead(transport, queued, DiagnosticDaqModeRegister, 0U);
    QueueLocalRead(transport, queued, 0x1104U, 0U);
    QueueLocalRead(transport, queued, 0x1204U, 0U);
    QueueLocalRead(transport, queued, 0x221CU, 0U);

    FakeDataReceiver dataReceiver;
    REQUIRE(dataReceiver.Open("mvlc-test", 32769U).success);
    prepared = StopDiagnosticAcquisition(
        transport,
        dataReceiver,
        std::move(prepared),
        request,
        cancelled);

    CHECK(prepared.acquisition.state == DiagnosticAcquisitionState::Stopped);
    CHECK(prepared.acquisition.moduleStopSent);
    CHECK(prepared.acquisition.daqModeDisabled);
    CHECK(prepared.acquisition.readoutStackDisabled);
    CHECK(prepared.acquisition.recoveryJournalRemoved);
    CHECK_FALSE(prepared.acquisition.recoveryJournalPrepared);
    CHECK_FALSE(prepared.acquisition.recoveryJournalActive);
    CHECK_FALSE(dataReceiver.IsOpen());
    CHECK_FALSE(std::filesystem::exists(journal.Get()));
    CHECK(prepared.nextSuperReference == queued.super);
    CHECK(prepared.nextStackReference == queued.stack);
}

TEST_CASE("takeover at stop detaches without cleanup and retains the journal")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueTargetValidation(transport, references);
    auto request = MakeRequest(journal.Get());
    request.configuredModuleBaseAddresses = {TargetBase};
    const std::atomic<bool> cancelled{false};
    auto prepared = PrepareDiagnosticAcquisition(
        transport, request, cancelled, AllowOwnership());
    REQUIRE(prepared.acquisition.recoveryJournalPrepared);
    prepared.acquisition.state = DiagnosticAcquisitionState::Running;
    prepared.acquisition.recoveryJournalActive = true;
    prepared.recoveryRecord.phase = TunerRecoveryPhase::Active;
    REQUIRE(SaveTunerRecoveryJournal(
        prepared.recoveryRecord, journal.Get()).success);

    const auto addresses = FingerprintAddresses(prepared);
    auto values = FingerprintValues(prepared);
    values[1] = 0U;
    QueueFingerprintRead(
        transport, prepared.nextSuperReference, addresses, values);
    const auto sendsBeforeStop = transport.SentRequests().size();

    FakeDataReceiver dataReceiver;
    REQUIRE(dataReceiver.Open("mvlc-test", 32769U).success);
    prepared = StopDiagnosticAcquisition(
        transport,
        dataReceiver,
        std::move(prepared),
        request,
        cancelled);

    CHECK(prepared.acquisition.state == DiagnosticAcquisitionState::Failed);
    CHECK(prepared.acquisition.foreignControllerDetected);
    CHECK(prepared.acquisition.cleanupSkippedToProtectForeignRun);
    CHECK_FALSE(prepared.acquisition.moduleStopSent);
    CHECK_FALSE(prepared.acquisition.recoveryJournalRemoved);
    CHECK(std::filesystem::exists(journal.Get()));
    CHECK_FALSE(dataReceiver.IsOpen());
    CHECK_FALSE(transport.IsOpen());
    CHECK(transport.SentRequests().size() == sendsBeforeStop + 1U);

    const auto operations = DecodeWireOperations(transport);
    for (const auto& operation : operations)
    {
        if (operation.write)
        {
            CHECK(operation.address
                  != TargetBase + DiagnosticAcquisitionControlRegister);
        }
    }
}

TEST_CASE("cleanup retains the journal when automatic preview restore failed")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences queued{0x5000U, 0x9E000001U};
    QueueTargetValidation(transport, queued);
    auto request = MakeRequest(journal.Get());
    request.configuredModuleBaseAddresses = {TargetBase};
    const std::atomic<bool> cancelled{false};
    auto prepared = PrepareDiagnosticAcquisition(
        transport, request, cancelled, AllowOwnership());
    prepared.acquisition.state = DiagnosticAcquisitionState::Running;
    prepared.acquisition.recoveryJournalActive = true;
    prepared.recoveryRecord.phase = TunerRecoveryPhase::Active;
    prepared.recoveryRecord.previewRestoreRequired = true;
    prepared.recoveryRecord.previewQuad = 7U;
    prepared.recoveryRecord.previewRegisterOffset = 0x611AU;
    prepared.recoveryRecord.previewOriginalValue = 200U;
    prepared.recoveryRecord.previewAppliedValue = 250U;
    REQUIRE(SaveTunerRecoveryJournal(
        prepared.recoveryRecord, journal.Get()).success);

    QueueWrite(
        transport,
        queued,
        TargetBase + DiagnosticAcquisitionControlRegister,
        0U);
    QueueRead(
        transport,
        queued,
        TargetBase + DiagnosticAcquisitionControlRegister,
        0U);
    QueueLocalWrite(
        transport,
        queued,
        {
            {DiagnosticDaqModeRegister, 0U},
            {0x1104U, 0U},
            {0x1204U, 0U},
            {0x221CU, 0U},
        });
    QueueLocalRead(transport, queued, DiagnosticDaqModeRegister, 0U);
    QueueLocalRead(transport, queued, 0x1104U, 0U);
    QueueLocalRead(transport, queued, 0x1204U, 0U);
    QueueLocalRead(transport, queued, 0x221CU, 0U);

    FakeDataReceiver dataReceiver;
    REQUIRE(dataReceiver.Open("mvlc-test", 32769U).success);
    prepared = StopDiagnosticAcquisition(
        transport,
        dataReceiver,
        std::move(prepared),
        request,
        cancelled,
        DiagnosticStopOwnershipCheck::
            VerifiedImmediatelyBeforePreviewRestore);

    CHECK(prepared.acquisition.state == DiagnosticAcquisitionState::Failed);
    CHECK(prepared.acquisition.moduleStopSent);
    CHECK(prepared.acquisition.daqModeDisabled);
    CHECK(prepared.acquisition.readoutStackDisabled);
    CHECK(prepared.acquisition.orphanRecoveryRequired);
    CHECK_FALSE(prepared.acquisition.recoveryJournalRemoved);
    CHECK(std::filesystem::exists(journal.Get()));
    CHECK(prepared.acquisition.message.find(
              "temporary parameter preview could not be restored")
          != std::string::npos);
}

TEST_CASE("cleanup retains the journal when automatic source restore failed")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences queued{0x5000U, 0x9E000001U};
    QueueTargetValidation(transport, queued);
    auto request = MakeRequest(journal.Get());
    request.configuredModuleBaseAddresses = {TargetBase};
    const std::atomic<bool> cancelled{false};
    auto prepared = PrepareDiagnosticAcquisition(
        transport, request, cancelled, AllowOwnership());
    prepared.acquisition.state = DiagnosticAcquisitionState::Running;
    prepared.acquisition.recoveryJournalActive = true;
    prepared.recoveryRecord.phase = TunerRecoveryPhase::Active;
    prepared.recoveryRecord.sourceRestoreRequired = true;
    prepared.recoveryRecord.sourceQuad = 7U;
    prepared.recoveryRecord.sourceOriginalConfiguration = 0x0040U;
    prepared.recoveryRecord.sourceAppliedConfigurationAvailable = true;
    prepared.recoveryRecord.sourceAppliedConfiguration = 0x0043U;
    REQUIRE(SaveTunerRecoveryJournal(
        prepared.recoveryRecord, journal.Get()).success);

    QueueWrite(
        transport,
        queued,
        TargetBase + DiagnosticAcquisitionControlRegister,
        0U);
    QueueRead(
        transport,
        queued,
        TargetBase + DiagnosticAcquisitionControlRegister,
        0U);
    QueueLocalWrite(
        transport,
        queued,
        {
            {DiagnosticDaqModeRegister, 0U},
            {0x1104U, 0U},
            {0x1204U, 0U},
            {0x221CU, 0U},
        });
    QueueLocalRead(transport, queued, DiagnosticDaqModeRegister, 0U);
    QueueLocalRead(transport, queued, 0x1104U, 0U);
    QueueLocalRead(transport, queued, 0x1204U, 0U);
    QueueLocalRead(transport, queued, 0x221CU, 0U);

    FakeDataReceiver dataReceiver;
    REQUIRE(dataReceiver.Open("mvlc-test", 32769U).success);
    prepared = StopDiagnosticAcquisition(
        transport,
        dataReceiver,
        std::move(prepared),
        request,
        cancelled,
        DiagnosticStopOwnershipCheck::
            VerifiedImmediatelyBeforePreviewRestore);

    CHECK(prepared.acquisition.state == DiagnosticAcquisitionState::Failed);
    CHECK(prepared.acquisition.moduleStopSent);
    CHECK(prepared.acquisition.daqModeDisabled);
    CHECK(prepared.acquisition.readoutStackDisabled);
    CHECK(prepared.acquisition.orphanRecoveryRequired);
    CHECK_FALSE(prepared.acquisition.recoveryJournalRemoved);
    CHECK(std::filesystem::exists(journal.Get()));
    CHECK(prepared.acquisition.message.find(
              "temporary waveform source could not be restored")
          != std::string::npos);
}

TEST_CASE("a restarted isolated module is stopped and reset during cleanup")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences queued{0x5000U, 0x9E000001U};
    QueueTargetValidation(transport, queued);
    QueueIsolationCapture(transport, queued, 0U, 0U);
    QueueIsolationWrites(transport, queued, false);
    auto request = MakeRequest(journal.Get());
    const std::atomic<bool> cancelled{false};
    auto prepared = PrepareDiagnosticAcquisition(
        transport, request, cancelled, AllowOwnership());
    REQUIRE(prepared.acquisition.recoveryJournalPrepared);
    prepared.acquisition.state = DiagnosticAcquisitionState::Running;
    prepared.acquisition.recoveryJournalActive = true;
    prepared.recoveryRecord.phase = TunerRecoveryPhase::Active;
    REQUIRE(SaveTunerRecoveryJournal(
        prepared.recoveryRecord, journal.Get()).success);

    QueueFingerprintRead(
        transport,
        prepared.nextSuperReference,
        FingerprintAddresses(prepared),
        FingerprintValues(prepared));
    ++queued.super;
    QueueWrite(
        transport,
        queued,
        TargetBase + DiagnosticAcquisitionControlRegister,
        0U);
    QueueRead(
        transport,
        queued,
        TargetBase + DiagnosticAcquisitionControlRegister,
        0U);
    QueueRead(
        transport,
        queued,
        OtherBase + DiagnosticAcquisitionControlRegister,
        1U);
    QueueWrite(
        transport,
        queued,
        OtherBase + DiagnosticAcquisitionControlRegister,
        0U);
    QueueRead(
        transport,
        queued,
        OtherBase + DiagnosticAcquisitionControlRegister,
        0U);
    QueueWrite(
        transport,
        queued,
        OtherBase + DiagnosticFifoResetRegister,
        1U);
    QueueWrite(
        transport,
        queued,
        OtherBase + DiagnosticReadoutResetRegister,
        1U);
    QueueLocalWrite(
        transport,
        queued,
        {
            {DiagnosticDaqModeRegister, 0U},
            {0x1104U, 0U},
            {0x1204U, 0U},
            {0x221CU, 0U},
        });
    QueueLocalRead(transport, queued, DiagnosticDaqModeRegister, 0U);
    QueueLocalRead(transport, queued, 0x1104U, 0U);
    QueueLocalRead(transport, queued, 0x1204U, 0U);
    QueueLocalRead(transport, queued, 0x221CU, 0U);

    FakeDataReceiver dataReceiver;
    REQUIRE(dataReceiver.Open("mvlc-test", 32769U).success);
    prepared = StopDiagnosticAcquisition(
        transport,
        dataReceiver,
        std::move(prepared),
        request,
        cancelled);

    CHECK(prepared.acquisition.state == DiagnosticAcquisitionState::Stopped);
    CHECK(prepared.acquisition.nonTargetModulesVerifiedStoppedOnCleanup == 1U);
    REQUIRE(prepared.acquisition.moduleIsolation.size() == 1U);
    const auto& isolated = prepared.acquisition.moduleIsolation.front();
    CHECK(isolated.stopSent);
    CHECK(isolated.stopVerified);
    CHECK(isolated.fifoResetSent);
    CHECK(isolated.readoutResetSent);
    CHECK(isolated.cleanupVerified);
    CHECK(isolated.message
          == "Unexpectedly restarted, then stopped, reset, and verified "
             "during cleanup.");
}
