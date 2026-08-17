#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/RecoveryJournal.h"
#include "core/RecoveryVerification.h"
#include "hardware/DiagnosticRecoveryOperation.h"
#include "diagnostic_tune_test_support.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t OtherBase = 0x22000000U;
constexpr std::uint16_t SampleConfigurationRegister = 0x614AU;

class JournalPath
{
public:
    JournalPath()
    {
        const auto unique = std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count();
        path_ = (std::filesystem::temp_directory_path()
                 / ("fidget-orphan-recovery-" + std::to_string(unique)
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

fidget::TunerRecoveryRecord MakeRecord()
{
    auto session = fidget::test::MakeRunningDiagnosticSession();
    auto record = session.recoveryRecord;
    record.isolatedModuleBaseAddresses = {OtherBase};
    return record;
}

std::vector<std::uint16_t> FingerprintAddresses(
    const fidget::TunerRecoveryRecord& record)
{
    const auto expected =
        fidget::BuildTunerRecoveryFingerprintExpectation(record);
    REQUIRE(expected.success);
    return {expected.addresses.begin(), expected.addresses.end()};
}

std::vector<std::uint32_t> FingerprintValues(
    const fidget::TunerRecoveryRecord& record,
    const std::uint32_t daqMode = 0x00000005U)
{
    const auto expected =
        fidget::BuildTunerRecoveryFingerprintExpectation(record);
    REQUIRE(expected.success);
    auto values = std::vector<std::uint32_t>(
        expected.values.begin(), expected.values.end());
    values[0] = daqMode;
    return values;
}

void QueueRecoveryFingerprint(
    fidget::test::FakeCommandTransport& transport,
    fidget::test::TransactionReferences& references,
    const fidget::TunerRecoveryRecord& record,
    std::vector<std::uint32_t> values = {})
{
    using namespace fidget;
    using namespace fidget::test;

    QueueLocalBatchRead(
        transport,
        references.super++,
        {
            TunerRecoveryMvlcHardwareIdRegister,
            TunerRecoveryMvlcFirmwareRegister,
        },
        {record.mvlcHardwareId, record.mvlcFirmwareRevision});
    if (values.empty())
    {
        values = FingerprintValues(record);
    }
    QueueLocalBatchRead(
        transport,
        references.super++,
        FingerprintAddresses(record),
        values);
}

void QueueTargetIdentity(
    fidget::test::FakeCommandTransport& transport,
    fidget::test::TransactionReferences& references,
    const fidget::TunerRecoveryRecord& record,
    const std::uint16_t hardwareId)
{
    fidget::test::QueueRead(
        transport,
        references,
        record.mdppBaseAddress + fidget::DiagnosticHardwareIdRegister,
        hardwareId);
}

void QueueLocalCleanup(
    fidget::test::FakeCommandTransport& transport,
    fidget::test::TransactionReferences& references,
    const fidget::TunerRecoveryRecord& record)
{
    using namespace fidget;
    using namespace fidget::test;

    const std::vector<MvlcLocalRegisterWrite> writes{
        {TunerRecoveryDaqModeRegister, 0U},
        {record.stackTriggerRegister, 0U},
        {record.stackOffsetRegister, 0U},
        {record.ownershipTokenRegister, 0U},
    };
    const auto request = BuildMvlcLocalRegisterWriteRequest(
        references.super, writes.data(), writes.size());
    REQUIRE(request.success);
    transport.QueueExchange({
        EncodeWords(request.words),
        {FakeReceiveAction::Datagram(
            MakeCommandPacket({MakeSuperFrame(references.super)}))},
    });
    ++references.super;
    for (const auto& write : writes)
    {
        QueueLocalRead(transport, references, write.address, 0U);
    }
}

bool ContainsAnyWrite(const fidget::test::FakeCommandTransport& transport)
{
    using namespace fidget;
    using namespace fidget::test;

    for (const auto& request : transport.SentRequests())
    {
        const auto words = DecodeWords(request);
        for (const auto word : words)
        {
            if (word == MvlcVmeWriteA32D16Command
                || (word & 0xFFFF0000U) == MvlcWriteLocalCommand)
            {
                return true;
            }
        }
    }
    return false;
}

} // namespace

TEST_CASE("confirmed orphans execute the complete verified recovery sequence")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    auto record = MakeRecord();
    record.previewRestoreRequired = true;
    record.previewQuad = 7U;
    record.previewRegisterOffset = 0x611AU;
    record.previewOriginalValue = 200U;
    record.previewAppliedValue = 250U;
    record.sourceRestoreRequired = true;
    record.sourceQuad = 7U;
    record.sourceOriginalConfiguration = 0x0040U;
    REQUIRE(SaveTunerRecoveryJournal(record, journal.Get()).success);

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueRecoveryFingerprint(transport, references, record);
    QueueTargetIdentity(
        transport,
        references,
        record,
        static_cast<std::uint16_t>(record.mdppHardwareId));
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + DiagnosticAcquisitionControlRegister,
        0U);
    QueueRead(
        transport,
        references,
        DiagnosticTestBase + DiagnosticAcquisitionControlRegister,
        0U);
    QueueWrite(
        transport,
        references,
        OtherBase + DiagnosticAcquisitionControlRegister,
        0U);
    QueueRead(
        transport,
        references,
        OtherBase + DiagnosticAcquisitionControlRegister,
        0U);
    QueueWrite(
        transport, references, OtherBase + DiagnosticFifoResetRegister, 1U);
    QueueWrite(
        transport,
        references,
        OtherBase + DiagnosticReadoutResetRegister,
        1U);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        7U);
    QueueRead(
        transport,
        references,
        DiagnosticTestBase + 0x611AU,
        250U);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + 0x611AU,
        200U);
    QueueRead(
        transport,
        references,
        DiagnosticTestBase + 0x611AU,
        200U);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        0U);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        7U);
    QueueRead(
        transport,
        references,
        DiagnosticTestBase + SampleConfigurationRegister,
        0x0043U);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + SampleConfigurationRegister,
        0x0040U);
    QueueRead(
        transport,
        references,
        DiagnosticTestBase + SampleConfigurationRegister,
        0x0040U);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        0U);
    QueueLocalCleanup(transport, references, record);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + DiagnosticFifoResetRegister,
        1U);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + DiagnosticReadoutResetRegister,
        1U);

    const std::atomic<bool> cancelled{false};
    const auto result = RecoverDiagnosticOrphan(
        transport, {record, journal.Get()}, cancelled);

    CHECK(result.state == DiagnosticOrphanRecoveryState::Recovered);
    CHECK(result.targetStopped);
    CHECK(result.isolatedModulesRecovered == 1U);
    CHECK(result.previewRestoreAttempted);
    CHECK(result.previewRestoreVerified);
    CHECK(result.sourceRestoreAttempted);
    CHECK(result.sourceRestoreVerified);
    CHECK(result.mvlcCleanupVerified);
    CHECK(result.targetReset);
    CHECK(result.journalRemoved);
    CHECK_FALSE(std::filesystem::exists(journal.Get()));
    CHECK(transport.SentRequests().size() == 45U);

    const auto operations = DecodeWireOperations(transport);
    std::vector<std::pair<std::uint32_t, std::uint16_t>> writes;
    for (const auto& operation : operations)
    {
        if (operation.write)
        {
            writes.emplace_back(operation.address, operation.value);
        }
    }
    const std::vector<std::pair<std::uint32_t, std::uint16_t>> expectedWrites{
        {DiagnosticTestBase + DiagnosticAcquisitionControlRegister, 0U},
        {OtherBase + DiagnosticAcquisitionControlRegister, 0U},
        {OtherBase + DiagnosticFifoResetRegister, 1U},
        {OtherBase + DiagnosticReadoutResetRegister, 1U},
        {DiagnosticTestBase + Fw2051ScpSelectorRegister, 7U},
        {DiagnosticTestBase + 0x611AU, 200U},
        {DiagnosticTestBase + Fw2051ScpSelectorRegister, 0U},
        {DiagnosticTestBase + Fw2051ScpSelectorRegister, 7U},
        {DiagnosticTestBase + SampleConfigurationRegister, 0x0040U},
        {DiagnosticTestBase + Fw2051ScpSelectorRegister, 0U},
        {DiagnosticTestBase + DiagnosticFifoResetRegister, 1U},
        {DiagnosticTestBase + DiagnosticReadoutResetRegister, 1U},
    };
    CHECK(writes == expectedWrites);
}

TEST_CASE("a token mismatch refuses every recovery write")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    const auto record = MakeRecord();
    REQUIRE(SaveTunerRecoveryJournal(record, journal.Get()).success);
    auto values = FingerprintValues(record);
    values.back() ^= 1U;

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueRecoveryFingerprint(transport, references, record, values);
    const std::atomic<bool> cancelled{false};
    const auto result = RecoverDiagnosticOrphan(
        transport, {record, journal.Get()}, cancelled);

    CHECK(result.state
          == DiagnosticOrphanRecoveryState::ForeignOrMismatched);
    CHECK(result.fingerprint.firstMismatchedField
          == "Unique tuner ownership token");
    CHECK_FALSE(result.hardwareWriteSent);
    CHECK_FALSE(ContainsAnyWrite(transport));
    CHECK(std::filesystem::exists(journal.Get()));
}

TEST_CASE("a target MDPP identity mismatch refuses every recovery write")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    const auto record = MakeRecord();
    REQUIRE(SaveTunerRecoveryJournal(record, journal.Get()).success);

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueRecoveryFingerprint(transport, references, record);
    QueueTargetIdentity(transport, references, record, 0x5008U);

    const std::atomic<bool> cancelled{false};
    const auto result = RecoverDiagnosticOrphan(
        transport, {record, journal.Get()}, cancelled);

    CHECK(result.state == DiagnosticOrphanRecoveryState::Failed);
    CHECK(result.message.find("MDPP hardware ID mismatch")
          != std::string::npos);
    CHECK_FALSE(result.hardwareWriteSent);
    const auto operations = DecodeWireOperations(transport);
    CHECK(std::none_of(
        operations.begin(),
        operations.end(),
        [](const WireOperation& operation) { return operation.write; }));
    CHECK(std::filesystem::exists(journal.Get()));
}

TEST_CASE("DAQ-idle recovery removes a stale journal without writes")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    const auto record = MakeRecord();
    REQUIRE(SaveTunerRecoveryJournal(record, journal.Get()).success);

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueRecoveryFingerprint(
        transport, references, record, FingerprintValues(record, 0U));
    const std::atomic<bool> cancelled{false};
    const auto result = RecoverDiagnosticOrphan(
        transport, {record, journal.Get()}, cancelled);

    CHECK(result.state == DiagnosticOrphanRecoveryState::AlreadyClean);
    CHECK(result.journalRemoved);
    CHECK_FALSE(result.hardwareWriteSent);
    CHECK_FALSE(ContainsAnyWrite(transport));
    CHECK_FALSE(std::filesystem::exists(journal.Get()));
}

TEST_CASE("unexpected preview values are parked but never overwritten")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    auto record = MakeRecord();
    record.isolatedModuleBaseAddresses.clear();
    record.previewRestoreRequired = true;
    record.previewQuad = 7U;
    record.previewRegisterOffset = 0x611AU;
    record.previewOriginalValue = 200U;
    record.previewAppliedValue = 250U;
    REQUIRE(SaveTunerRecoveryJournal(record, journal.Get()).success);

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueRecoveryFingerprint(transport, references, record);
    QueueTargetIdentity(
        transport,
        references,
        record,
        static_cast<std::uint16_t>(record.mdppHardwareId));
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + DiagnosticAcquisitionControlRegister,
        0U);
    QueueRead(
        transport,
        references,
        DiagnosticTestBase + DiagnosticAcquisitionControlRegister,
        0U);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        7U);
    QueueRead(
        transport,
        references,
        DiagnosticTestBase + 0x611AU,
        251U);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        0U);

    const std::atomic<bool> cancelled{false};
    const auto result = RecoverDiagnosticOrphan(
        transport, {record, journal.Get()}, cancelled);

    CHECK(result.state == DiagnosticOrphanRecoveryState::Failed);
    CHECK_FALSE(result.previewRestoreAttempted);
    CHECK(result.message.find("refused to overwrite") != std::string::npos);
    CHECK(std::filesystem::exists(journal.Get()));
    const auto operations = DecodeWireOperations(transport);
    CHECK(std::none_of(
        operations.begin(),
        operations.end(),
        [](const WireOperation& operation) {
            return operation.write
                && operation.address == DiagnosticTestBase + 0x611AU;
        }));
}

TEST_CASE("an already-restored preview completes orphan recovery")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    auto record = MakeRecord();
    record.isolatedModuleBaseAddresses.clear();
    record.previewRestoreRequired = true;
    record.previewQuad = 7U;
    record.previewRegisterOffset = 0x611AU;
    record.previewOriginalValue = 200U;
    record.previewAppliedValue = 250U;
    REQUIRE(SaveTunerRecoveryJournal(record, journal.Get()).success);

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueRecoveryFingerprint(transport, references, record);
    QueueTargetIdentity(
        transport,
        references,
        record,
        static_cast<std::uint16_t>(record.mdppHardwareId));
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + DiagnosticAcquisitionControlRegister,
        0U);
    QueueRead(
        transport,
        references,
        DiagnosticTestBase + DiagnosticAcquisitionControlRegister,
        0U);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        7U);
    QueueRead(
        transport,
        references,
        DiagnosticTestBase + 0x611AU,
        200U);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        0U);
    QueueLocalCleanup(transport, references, record);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + DiagnosticFifoResetRegister,
        1U);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + DiagnosticReadoutResetRegister,
        1U);

    const std::atomic<bool> cancelled{false};
    const auto result = RecoverDiagnosticOrphan(
        transport, {record, journal.Get()}, cancelled);

    CHECK(result.state == DiagnosticOrphanRecoveryState::Recovered);
    CHECK_FALSE(result.previewRestoreAttempted);
    CHECK(result.previewRestoreVerified);
    CHECK(result.journalRemoved);
    CHECK_FALSE(std::filesystem::exists(journal.Get()));
    const auto operations = DecodeWireOperations(transport);
    CHECK(std::none_of(
        operations.begin(),
        operations.end(),
        [](const WireOperation& operation) {
            return operation.write
                && operation.address == DiagnosticTestBase + 0x611AU;
        }));
}

TEST_CASE("cancellation during recovery retains the journal")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    auto record = MakeRecord();
    record.isolatedModuleBaseAddresses.clear();
    REQUIRE(SaveTunerRecoveryJournal(record, journal.Get()).success);

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueRecoveryFingerprint(transport, references, record);
    QueueTargetIdentity(
        transport,
        references,
        record,
        static_cast<std::uint16_t>(record.mdppHardwareId));
    std::atomic<bool> cancelled{false};
    std::size_t sends = 0U;
    transport.SetSendHook([&cancelled, &sends](const auto&) {
        ++sends;
        if (sends == 3U)
        {
            cancelled.store(true);
        }
    });

    const auto result = RecoverDiagnosticOrphan(
        transport, {record, journal.Get()}, cancelled);

    CHECK(result.state == DiagnosticOrphanRecoveryState::Failed);
    CHECK(result.message.find("cancelled") != std::string::npos);
    CHECK_FALSE(result.journalRemoved);
    CHECK(std::filesystem::exists(journal.Get()));
}
