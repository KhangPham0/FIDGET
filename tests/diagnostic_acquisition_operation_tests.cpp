#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/RecoveryJournal.h"
#include "hardware/DiagnosticAcquisitionOperation.h"
#include "vme_test_support.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

namespace {

constexpr std::uint32_t TargetBase = 0x11000000U;
constexpr std::uint32_t OtherBase = 0x22000000U;
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
    const auto prepared = PrepareDiagnosticAcquisition(
        transport,
        MakeRequest(journal.Get()),
        cancelled,
        AllowOwnership());

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

TEST_CASE("acquisition cannot begin when recovery journaling is unavailable")
{
    using namespace fidget;
    using namespace fidget::test;

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueTargetValidation(transport, references);
    QueueIsolationCapture(transport, references, 0U, 0U);
    QueueIsolationWrites(transport, references, false);

    auto request = MakeRequest("/dev/null/fidget.recovery");
    const std::atomic<bool> cancelled{false};
    const auto prepared = PrepareDiagnosticAcquisition(
        transport, request, cancelled, AllowOwnership());

    CHECK(prepared.acquisition.state == DiagnosticAcquisitionState::Failed);
    CHECK_FALSE(prepared.acquisition.recoveryJournalPrepared);
    CHECK(prepared.acquisition.message.find(
              "Could not prepare crash recovery before installing the "
              "diagnostic stack:")
          == 0U);

    const auto operations = DecodeWireOperations(transport);
    for (const auto& operation : operations)
    {
        if (!operation.write)
        {
            continue;
        }
        CHECK(operation.address != 0x2200U);
        CHECK(operation.address != 0x1104U);
        CHECK(operation.address != 0x1204U);
    }
}
