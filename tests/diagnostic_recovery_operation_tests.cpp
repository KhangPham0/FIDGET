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
    const std::uint16_t hardwareId,
    const std::uint16_t firmwareRevision =
        fidget::Mdpp32ScpFirmwareRevisionFw2051)
{
    fidget::test::QueueRead(
        transport,
        references,
        record.mdppBaseAddress + fidget::DiagnosticHardwareIdRegister,
        hardwareId);
    fidget::test::QueueRead(
        transport,
        references,
        record.mdppBaseAddress + 0x600EU,
        firmwareRevision);
}

void QueueIdleDaqGate(
    fidget::test::FakeCommandTransport& transport,
    fidget::test::TransactionReferences& references,
    const std::uint32_t daqMode = 0U)
{
    fidget::test::QueueLocalRead(
        transport,
        references,
        fidget::TunerRecoveryDaqModeRegister,
        daqMode);
}

void QueueIdleIdentityGauntlet(
    fidget::test::FakeCommandTransport& transport,
    fidget::test::TransactionReferences& references,
    const fidget::TunerRecoveryRecord& record,
    const std::uint16_t hardwareId,
    const std::uint16_t firmwareRevision =
        fidget::Mdpp32ScpFirmwareRevisionFw2051,
    const std::uint16_t acquisitionState = 0U)
{
    QueueIdleDaqGate(transport, references);
    QueueTargetIdentity(
        transport,
        references,
        record,
        hardwareId,
        firmwareRevision);
    fidget::test::QueueRead(
        transport,
        references,
        record.mdppBaseAddress
            + fidget::DiagnosticAcquisitionControlRegister,
        acquisitionState);
}

void QueueFailedWrite(
    fidget::test::FakeCommandTransport& transport,
    fidget::test::TransactionReferences& references,
    const std::uint32_t address,
    const std::uint16_t value)
{
    using namespace fidget;
    using namespace fidget::test;

    const auto operation = EncodeMvlcVmeWriteD16Words(address, value);
    const auto upload = BuildMvlcStackUploadRequest(
        references.super,
        references.stack,
        operation.data(),
        operation.size());
    transport.QueueExchange({
        EncodeWords(upload),
        {FakeReceiveAction::Datagram(
            MakeCommandPacket({MakeSuperFrame(references.super)}))},
    });
    ++references.super;
    const std::vector<std::uint32_t> failedStackFrame{
        (static_cast<std::uint32_t>(MvlcStackFrameType) << 24U)
            | (static_cast<std::uint32_t>(MvlcBusErrorFlag) << 20U)
            | 1U,
        references.stack,
    };
    transport.QueueExchange({
        EncodeWords(BuildMvlcStackExecuteRequest(references.super)),
        {FakeReceiveAction::Datagram(MakeCommandPacket({
            MakeSuperFrame(references.super),
            failedStackFrame,
        }))},
    });
    ++references.super;
    ++references.stack;
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

bool ContainsLocalWriteTo(
    const fidget::test::FakeCommandTransport& transport,
    const std::uint16_t address)
{
    using namespace fidget;
    using namespace fidget::test;

    for (const auto& request : transport.SentRequests())
    {
        const auto words = DecodeWords(request);
        if (std::find(
                words.begin(),
                words.end(),
                MvlcWriteLocalCommand | address)
            != words.end())
        {
            return true;
        }
    }
    return false;
}

void CheckWireOperations(
    const std::vector<fidget::test::WireOperation>& actual,
    const std::vector<fidget::test::WireOperation>& expected)
{
    REQUIRE(actual.size() == expected.size());
    for (std::size_t index = 0U; index < expected.size(); ++index)
    {
        INFO(index);
        CHECK(actual[index].write == expected[index].write);
        CHECK(actual[index].address == expected[index].address);
        CHECK(actual[index].value == expected[index].value);
    }
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
    record.sourceAppliedConfigurationAvailable = true;
    record.sourceAppliedConfiguration = 0x0043U;
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
    QueueRead(
        transport,
        references,
        OtherBase + DiagnosticHardwareIdRegister,
        Mdpp32HardwareId);
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
    CHECK(transport.SentRequests().size() == 49U);

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
    CHECK(transport.SentRequests().size() == 2U);
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

TEST_CASE("read-only MDPP-32 v2 identity evidence does not authorize recovery writes")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    auto record = MakeRecord();
    record.mdppHardwareId = Mdpp32AlternateHardwareId;
    REQUIRE(SaveTunerRecoveryJournal(record, journal.Get()).success);

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueRecoveryFingerprint(transport, references, record);
    QueueTargetIdentity(
        transport,
        references,
        record,
        Mdpp32AlternateHardwareId,
        Mdpp32ScpFirmwareRevisionFw2051);

    const std::atomic<bool> cancelled{false};
    const auto result = RecoverDiagnosticOrphan(
        transport, {record, journal.Get()}, cancelled);

    INFO(result.message);
    CHECK(result.state == DiagnosticOrphanRecoveryState::Failed);
    CHECK(result.message.find("await recorded hardware acceptance")
          != std::string::npos);
    CHECK_FALSE(result.hardwareWriteSent);
    const auto operations = DecodeWireOperations(transport);
    CHECK(std::none_of(
        operations.begin(),
        operations.end(),
        [](const WireOperation& operation) { return operation.write; }));
    CHECK(std::filesystem::exists(journal.Get()));
}

TEST_CASE("an untested MDPP firmware refuses active-orphan recovery writes")
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
    QueueTargetIdentity(
        transport,
        references,
        record,
        record.mdppHardwareId,
        0x2050U);

    const std::atomic<bool> cancelled{false};
    const auto result = RecoverDiagnosticOrphan(
        transport, {record, journal.Get()}, cancelled);

    CHECK(result.state == DiagnosticOrphanRecoveryState::Failed);
    CHECK(result.message.find("expected exact FW2051")
          != std::string::npos);
    CHECK_FALSE(result.hardwareWriteSent);
    const auto operations = DecodeWireOperations(transport);
    CHECK(std::none_of(
        operations.begin(),
        operations.end(),
        [](const WireOperation& operation) { return operation.write; }));
    CHECK(std::filesystem::exists(journal.Get()));
}

TEST_CASE("a changed isolated-module identity refuses every recovery write")
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
    QueueTargetIdentity(
        transport,
        references,
        record,
        record.mdppHardwareId);
    QueueRead(
        transport,
        references,
        OtherBase + DiagnosticHardwareIdRegister,
        0x5008U);

    const std::atomic<bool> cancelled{false};
    const auto result = RecoverDiagnosticOrphan(
        transport, {record, journal.Get()}, cancelled);

    CHECK(result.state == DiagnosticOrphanRecoveryState::Failed);
    CHECK(result.message.find("isolated-module hardware ID")
          != std::string::npos);
    CHECK_FALSE(result.hardwareWriteSent);
    const auto operations = DecodeWireOperations(transport);
    CHECK(std::none_of(
        operations.begin(),
        operations.end(),
        [](const WireOperation& operation) { return operation.write; }));
    CHECK(std::filesystem::exists(journal.Get()));
}

TEST_CASE("a legacy v3 source also refuses active-orphan recovery writes")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    auto record = MakeRecord();
    record.formatVersion = 3U;
    record.sourceRestoreRequired = true;
    record.sourceQuad = 7U;
    record.sourceOriginalConfiguration = 0x0040U;
    record.sourceAppliedConfigurationAvailable = false;
    record.sourceAppliedConfiguration = 0U;
    REQUIRE(SaveTunerRecoveryJournal(record, journal.Get()).success);

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueRecoveryFingerprint(transport, references, record);
    QueueTargetIdentity(
        transport,
        references,
        record,
        record.mdppHardwareId);

    const std::atomic<bool> cancelled{false};
    const auto result = RecoverDiagnosticOrphan(
        transport, {record, journal.Get()}, cancelled);

    CHECK(result.state == DiagnosticOrphanRecoveryState::Failed);
    CHECK(result.message.find("predates format version 4")
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

TEST_CASE("DAQ-idle recovery restores a coupled preview and source independently")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    auto record = MakeRecord();
    record.previewRestoreRequired = true;
    record.previewQuad = 7U;
    record.previewRegisterOffset = 0x6148U;
    record.previewOriginalValue = 400U;
    record.previewAppliedValue = 480U;
    record.sourceRestoreRequired = true;
    record.sourceQuad = 7U;
    record.sourceOriginalConfiguration = 0x0040U;
    record.sourceAppliedConfigurationAvailable = true;
    record.sourceAppliedConfiguration = 0x0043U;
    REQUIRE(SaveTunerRecoveryJournal(record, journal.Get()).success);

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueRecoveryFingerprint(
        transport, references, record, FingerprintValues(record, 0U));
    QueueIdleIdentityGauntlet(
        transport, references, record, record.mdppHardwareId);

    QueueIdleDaqGate(transport, references);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        7U);
    QueueRead(
        transport, references, DiagnosticTestBase + 0x6148U, 480U);
    QueueRead(
        transport, references, DiagnosticTestBase + 0x6146U, 300U);
    QueueIdleDaqGate(transport, references);
    QueueWrite(
        transport, references, DiagnosticTestBase + 0x6148U, 400U);
    QueueRead(
        transport, references, DiagnosticTestBase + 0x6148U, 400U);
    QueueIdleDaqGate(transport, references);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        0U);

    QueueIdleDaqGate(transport, references);
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
    QueueIdleDaqGate(transport, references);
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
    QueueIdleDaqGate(transport, references);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        0U);

    const std::atomic<bool> cancelled{false};
    const auto result = RecoverDiagnosticOrphan(
        transport, {record, journal.Get()}, cancelled);

    CHECK(result.state == DiagnosticOrphanRecoveryState::Recovered);
    CHECK(result.fingerprint.verdict
          == TunerRecoveryFingerprintVerdict::IdleWithRestoration);
    CHECK(result.previewRestoreAttempted);
    CHECK(result.previewRestoreVerified);
    CHECK(result.sourceRestoreAttempted);
    CHECK(result.sourceRestoreVerified);
    CHECK(result.journalRemoved);
    CHECK_FALSE(result.targetStopped);
    CHECK(result.isolatedModulesRecovered == 0U);
    CHECK_FALSE(result.mvlcCleanupVerified);
    CHECK_FALSE(result.targetReset);
    CHECK_FALSE(ContainsLocalWriteTo(
        transport, TunerRecoveryDaqModeRegister));
    CHECK_FALSE(ContainsLocalWriteTo(
        transport, record.stackTriggerRegister));
    CHECK_FALSE(ContainsLocalWriteTo(
        transport, record.stackOffsetRegister));
    CHECK_FALSE(ContainsLocalWriteTo(
        transport, record.ownershipTokenRegister));
    CHECK_FALSE(std::filesystem::exists(journal.Get()));

    CheckWireOperations(
        DecodeWireOperations(transport),
        {
            {false, DiagnosticTestBase + DiagnosticHardwareIdRegister, 0U},
            {false, DiagnosticTestBase + 0x600EU, 0U},
            {false,
             DiagnosticTestBase + DiagnosticAcquisitionControlRegister,
             0U},
            {true,
             DiagnosticTestBase + Fw2051ScpSelectorRegister,
             7U},
            {false, DiagnosticTestBase + 0x6148U, 0U},
            {false, DiagnosticTestBase + 0x6146U, 0U},
            {true, DiagnosticTestBase + 0x6148U, 400U},
            {false, DiagnosticTestBase + 0x6148U, 0U},
            {true,
             DiagnosticTestBase + Fw2051ScpSelectorRegister,
             0U},
            {true,
             DiagnosticTestBase + Fw2051ScpSelectorRegister,
             7U},
            {false,
             DiagnosticTestBase + SampleConfigurationRegister,
             0U},
            {true,
             DiagnosticTestBase + SampleConfigurationRegister,
             0x0040U},
            {false,
             DiagnosticTestBase + SampleConfigurationRegister,
             0U},
            {true,
             DiagnosticTestBase + Fw2051ScpSelectorRegister,
             0U},
        });
}

TEST_CASE("idle recovery resaves a completed preview before a source refusal")
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
    record.sourceAppliedConfigurationAvailable = true;
    record.sourceAppliedConfiguration = 0x0043U;
    REQUIRE(SaveTunerRecoveryJournal(record, journal.Get()).success);

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueRecoveryFingerprint(
        transport, references, record, FingerprintValues(record, 0U));
    QueueIdleIdentityGauntlet(
        transport, references, record, record.mdppHardwareId);

    QueueIdleDaqGate(transport, references);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        7U);
    QueueRead(
        transport, references, DiagnosticTestBase + 0x611AU, 250U);
    QueueIdleDaqGate(transport, references);
    QueueWrite(
        transport, references, DiagnosticTestBase + 0x611AU, 200U);
    QueueRead(
        transport, references, DiagnosticTestBase + 0x611AU, 200U);
    QueueIdleDaqGate(transport, references);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        0U);

    QueueIdleDaqGate(transport, references);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        7U);
    QueueRead(
        transport,
        references,
        DiagnosticTestBase + SampleConfigurationRegister,
        0x0042U);
    QueueIdleDaqGate(transport, references);
    QueueIdleDaqGate(transport, references);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        0U);

    const std::atomic<bool> cancelled{false};
    const auto result = RecoverDiagnosticOrphan(
        transport, {record, journal.Get()}, cancelled);

    CHECK(result.state == DiagnosticOrphanRecoveryState::Failed);
    CHECK(result.previewRestoreVerified);
    CHECK_FALSE(result.sourceRestoreAttempted);
    CHECK_FALSE(result.journalRemoved);
    const auto retained = LoadTunerRecoveryJournal(journal.Get());
    REQUIRE(retained.success);
    REQUIRE(retained.record.has_value());
    CHECK_FALSE(retained.record->previewRestoreRequired);
    CHECK(retained.record->sourceRestoreRequired);
    CHECK(retained.record->sourceQuad == 7U);
    CHECK(retained.record->sourceOriginalConfiguration == 0x0040U);
    CHECK(retained.record->sourceAppliedConfigurationAvailable);
    CHECK(retained.record->sourceAppliedConfiguration == 0x0043U);
}

TEST_CASE("DAQ-idle recovery accepts an already-restored preview without rewriting it")
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
    REQUIRE(SaveTunerRecoveryJournal(record, journal.Get()).success);

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueRecoveryFingerprint(
        transport, references, record, FingerprintValues(record, 0U));
    QueueIdleIdentityGauntlet(
        transport, references, record, record.mdppHardwareId);
    QueueIdleDaqGate(transport, references);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        7U);
    QueueRead(
        transport, references, DiagnosticTestBase + 0x611AU, 200U);
    QueueIdleDaqGate(transport, references);
    QueueIdleDaqGate(transport, references);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        0U);

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

TEST_CASE("DAQ-idle recovery accepts an already-restored source without rewriting it")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    auto record = MakeRecord();
    record.sourceRestoreRequired = true;
    record.sourceQuad = 7U;
    record.sourceOriginalConfiguration = 0x0040U;
    record.sourceAppliedConfigurationAvailable = true;
    record.sourceAppliedConfiguration = 0x0043U;
    REQUIRE(SaveTunerRecoveryJournal(record, journal.Get()).success);

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueRecoveryFingerprint(
        transport, references, record, FingerprintValues(record, 0U));
    QueueIdleIdentityGauntlet(
        transport, references, record, record.mdppHardwareId);
    QueueIdleDaqGate(transport, references);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        7U);
    QueueRead(
        transport,
        references,
        DiagnosticTestBase + SampleConfigurationRegister,
        0x0040U);
    QueueIdleDaqGate(transport, references);
    QueueIdleDaqGate(transport, references);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        0U);

    const std::atomic<bool> cancelled{false};
    const auto result = RecoverDiagnosticOrphan(
        transport, {record, journal.Get()}, cancelled);

    CHECK(result.state == DiagnosticOrphanRecoveryState::Recovered);
    CHECK_FALSE(result.sourceRestoreAttempted);
    CHECK(result.sourceRestoreVerified);
    CHECK(result.journalRemoved);
    CHECK_FALSE(std::filesystem::exists(journal.Get()));
    const auto operations = DecodeWireOperations(transport);
    CHECK(std::none_of(
        operations.begin(),
        operations.end(),
        [](const WireOperation& operation) {
            return operation.write
                && operation.address
                    == DiagnosticTestBase + SampleConfigurationRegister;
        }));
}

TEST_CASE("DAQ-idle recovery parks but refuses an unexpected preview value")
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
    REQUIRE(SaveTunerRecoveryJournal(record, journal.Get()).success);

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueRecoveryFingerprint(
        transport, references, record, FingerprintValues(record, 0U));
    QueueIdleIdentityGauntlet(
        transport, references, record, record.mdppHardwareId);
    QueueIdleDaqGate(transport, references);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        7U);
    QueueRead(
        transport, references, DiagnosticTestBase + 0x611AU, 251U);
    QueueIdleDaqGate(transport, references);
    QueueIdleDaqGate(transport, references);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        0U);

    const std::atomic<bool> cancelled{false};
    const auto result = RecoverDiagnosticOrphan(
        transport, {record, journal.Get()}, cancelled);

    CHECK(result.state == DiagnosticOrphanRecoveryState::Failed);
    CHECK(result.message.find("unexpected value") != std::string::npos);
    CHECK_FALSE(result.previewRestoreAttempted);
    CHECK_FALSE(result.journalRemoved);
    CHECK(std::filesystem::exists(journal.Get()));
    const auto operations = DecodeWireOperations(transport);
    CHECK(std::none_of(
        operations.begin(),
        operations.end(),
        [](const WireOperation& operation) {
            return operation.write
                && operation.address == DiagnosticTestBase + 0x611AU;
        }));
    REQUIRE_FALSE(operations.empty());
    CHECK(operations.back().write);
    CHECK(operations.back().address
          == DiagnosticTestBase + Fw2051ScpSelectorRegister);
    CHECK(operations.back().value == 0U);
}

TEST_CASE("DAQ-idle recovery refuses a preview restore invalidated by its live dependency")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    auto record = MakeRecord();
    record.previewRestoreRequired = true;
    record.previewQuad = 7U;
    record.previewRegisterOffset = 0x6148U;
    record.previewOriginalValue = 400U;
    record.previewAppliedValue = 480U;
    REQUIRE(SaveTunerRecoveryJournal(record, journal.Get()).success);

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueRecoveryFingerprint(
        transport, references, record, FingerprintValues(record, 0U));
    QueueIdleIdentityGauntlet(
        transport, references, record, record.mdppHardwareId);
    QueueIdleDaqGate(transport, references);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        7U);
    QueueRead(
        transport, references, DiagnosticTestBase + 0x6148U, 480U);
    QueueRead(
        transport, references, DiagnosticTestBase + 0x6146U, 450U);
    QueueIdleDaqGate(transport, references);
    QueueIdleDaqGate(transport, references);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        0U);

    const std::atomic<bool> cancelled{false};
    const auto result = RecoverDiagnosticOrphan(
        transport, {record, journal.Get()}, cancelled);

    CHECK(result.state == DiagnosticOrphanRecoveryState::Failed);
    CHECK(result.message.find("now-invalid restore") != std::string::npos);
    CHECK(result.message.find("Pre-samples") != std::string::npos);
    CHECK_FALSE(result.previewRestoreAttempted);
    CHECK_FALSE(result.journalRemoved);
    CHECK(std::filesystem::exists(journal.Get()));
    const auto operations = DecodeWireOperations(transport);
    CHECK(std::none_of(
        operations.begin(),
        operations.end(),
        [](const WireOperation& operation) {
            return operation.write
                && operation.address == DiagnosticTestBase + 0x6148U;
        }));
    REQUIRE_FALSE(operations.empty());
    CHECK(operations.back().write);
    CHECK(operations.back().address
          == DiagnosticTestBase + Fw2051ScpSelectorRegister);
    CHECK(operations.back().value == 0U);
}

TEST_CASE("a parking failure is reported alongside an unexpected live value")
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
    REQUIRE(SaveTunerRecoveryJournal(record, journal.Get()).success);

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueRecoveryFingerprint(
        transport, references, record, FingerprintValues(record, 0U));
    QueueIdleIdentityGauntlet(
        transport, references, record, record.mdppHardwareId);
    QueueIdleDaqGate(transport, references);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        7U);
    QueueRead(
        transport, references, DiagnosticTestBase + 0x611AU, 251U);
    QueueIdleDaqGate(transport, references);
    QueueIdleDaqGate(transport, references);
    QueueFailedWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        0U);

    const std::atomic<bool> cancelled{false};
    const auto result = RecoverDiagnosticOrphan(
        transport, {record, journal.Get()}, cancelled);

    CHECK(result.state == DiagnosticOrphanRecoveryState::Failed);
    CHECK(result.message.find("unexpected value") != std::string::npos);
    CHECK(result.message.find("Could not park the recovery selector")
          != std::string::npos);
    CHECK_FALSE(result.previewRestoreAttempted);
    CHECK_FALSE(result.journalRemoved);
    CHECK(std::filesystem::exists(journal.Get()));
}

TEST_CASE("a legacy v3 source restoration refuses before any recovery write")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    auto record = MakeRecord();
    record.formatVersion = 3U;
    record.sourceRestoreRequired = true;
    record.sourceQuad = 7U;
    record.sourceOriginalConfiguration = 0x0040U;
    record.sourceAppliedConfigurationAvailable = false;
    record.sourceAppliedConfiguration = 0U;
    REQUIRE(SaveTunerRecoveryJournal(record, journal.Get()).success);

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueRecoveryFingerprint(
        transport, references, record, FingerprintValues(record, 0U));
    QueueIdleDaqGate(transport, references);
    QueueTargetIdentity(
        transport,
        references,
        record,
        record.mdppHardwareId);

    const std::atomic<bool> cancelled{false};
    const auto result = RecoverDiagnosticOrphan(
        transport, {record, journal.Get()}, cancelled);

    CHECK(result.state == DiagnosticOrphanRecoveryState::Failed);
    CHECK(result.message.find("predates format version 4")
          != std::string::npos);
    CHECK(result.message.find("resolve the source setting manually")
          != std::string::npos);
    CHECK_FALSE(result.hardwareWriteSent);
    const auto operations = DecodeWireOperations(transport);
    CHECK(std::none_of(
        operations.begin(),
        operations.end(),
        [](const WireOperation& operation) { return operation.write; }));
    CHECK(std::filesystem::exists(journal.Get()));
}

TEST_CASE("DAQ-idle recovery refuses an untested MDPP firmware before writes")
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
    REQUIRE(SaveTunerRecoveryJournal(record, journal.Get()).success);

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueRecoveryFingerprint(
        transport, references, record, FingerprintValues(record, 0U));
    QueueIdleDaqGate(transport, references);
    QueueTargetIdentity(
        transport,
        references,
        record,
        record.mdppHardwareId,
        0x2050U);

    const std::atomic<bool> cancelled{false};
    const auto result = RecoverDiagnosticOrphan(
        transport, {record, journal.Get()}, cancelled);

    CHECK(result.state == DiagnosticOrphanRecoveryState::Failed);
    CHECK(result.message.find("expected exact FW2051")
          != std::string::npos);
    CHECK_FALSE(result.hardwareWriteSent);
    const auto operations = DecodeWireOperations(transport);
    CHECK(std::none_of(
        operations.begin(),
        operations.end(),
        [](const WireOperation& operation) { return operation.write; }));
    CHECK(std::filesystem::exists(journal.Get()));
}

TEST_CASE("DAQ-idle recovery rejects a preview outside the FW2051 allowlist")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    auto record = MakeRecord();
    record.previewRestoreRequired = true;
    record.previewQuad = 7U;
    record.previewRegisterOffset = DiagnosticAcquisitionControlRegister;
    record.previewOriginalValue = 0U;
    record.previewAppliedValue = 1U;
    REQUIRE(SaveTunerRecoveryJournal(record, journal.Get()).success);

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueRecoveryFingerprint(
        transport, references, record, FingerprintValues(record, 0U));
    QueueIdleDaqGate(transport, references);
    QueueTargetIdentity(
        transport,
        references,
        record,
        record.mdppHardwareId);

    const std::atomic<bool> cancelled{false};
    const auto result = RecoverDiagnosticOrphan(
        transport, {record, journal.Get()}, cancelled);

    CHECK(result.state == DiagnosticOrphanRecoveryState::Failed);
    CHECK(result.message.find("not in the FW2051 SCP recovery allowlist")
          != std::string::npos);
    CHECK_FALSE(result.hardwareWriteSent);
    const auto operations = DecodeWireOperations(transport);
    CHECK(std::none_of(
        operations.begin(),
        operations.end(),
        [](const WireOperation& operation) { return operation.write; }));
    CHECK(std::filesystem::exists(journal.Get()));
}

TEST_CASE("DAQ-idle recovery requires the journaled MDPP to remain stopped")
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
    REQUIRE(SaveTunerRecoveryJournal(record, journal.Get()).success);

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueRecoveryFingerprint(
        transport, references, record, FingerprintValues(record, 0U));
    QueueIdleIdentityGauntlet(
        transport,
        references,
        record,
        record.mdppHardwareId,
        Mdpp32ScpFirmwareRevisionFw2051,
        1U);

    const std::atomic<bool> cancelled{false};
    const auto result = RecoverDiagnosticOrphan(
        transport, {record, journal.Get()}, cancelled);

    CHECK(result.state == DiagnosticOrphanRecoveryState::Failed);
    CHECK(result.message.find("expected zero") != std::string::npos);
    CHECK_FALSE(result.hardwareWriteSent);
    const auto operations = DecodeWireOperations(transport);
    CHECK(std::none_of(
        operations.begin(),
        operations.end(),
        [](const WireOperation& operation) { return operation.write; }));
    CHECK(std::filesystem::exists(journal.Get()));
}

TEST_CASE("DAQ-idle recovery stops without parking after a proven takeover")
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
    REQUIRE(SaveTunerRecoveryJournal(record, journal.Get()).success);

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueRecoveryFingerprint(
        transport, references, record, FingerprintValues(record, 0U));
    QueueIdleIdentityGauntlet(
        transport, references, record, record.mdppHardwareId);
    QueueIdleDaqGate(transport, references);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        7U);
    QueueRead(
        transport, references, DiagnosticTestBase + 0x611AU, 250U);
    QueueIdleDaqGate(transport, references, 0x00000005U);

    const std::atomic<bool> cancelled{false};
    const auto result = RecoverDiagnosticOrphan(
        transport, {record, journal.Get()}, cancelled);

    CHECK(result.state == DiagnosticOrphanRecoveryState::Failed);
    CHECK(result.message.find("DAQ mode changed") != std::string::npos);
    CHECK(result.hardwareWriteSent);
    CHECK_FALSE(result.previewRestoreAttempted);
    const auto operations = DecodeWireOperations(transport);
    const auto writes = std::count_if(
        operations.begin(),
        operations.end(),
        [](const WireOperation& operation) { return operation.write; });
    CHECK(writes == 1U);
    REQUIRE_FALSE(operations.empty());
    const auto selected = std::find_if(
        operations.begin(),
        operations.end(),
        [](const WireOperation& operation) { return operation.write; });
    REQUIRE(selected != operations.end());
    CHECK(selected->address
          == DiagnosticTestBase + Fw2051ScpSelectorRegister);
    CHECK(selected->value == 7U);
    CHECK(std::filesystem::exists(journal.Get()));
}

TEST_CASE("takeover after a verified restore keeps the journal armed and skips parking")
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
    REQUIRE(SaveTunerRecoveryJournal(record, journal.Get()).success);

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueRecoveryFingerprint(
        transport, references, record, FingerprintValues(record, 0U));
    QueueIdleIdentityGauntlet(
        transport, references, record, record.mdppHardwareId);
    QueueIdleDaqGate(transport, references);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        7U);
    QueueRead(
        transport, references, DiagnosticTestBase + 0x611AU, 250U);
    QueueIdleDaqGate(transport, references);
    QueueWrite(
        transport, references, DiagnosticTestBase + 0x611AU, 200U);
    QueueRead(
        transport, references, DiagnosticTestBase + 0x611AU, 200U);
    QueueIdleDaqGate(transport, references, 0x00000005U);

    const std::atomic<bool> cancelled{false};
    const auto result = RecoverDiagnosticOrphan(
        transport, {record, journal.Get()}, cancelled);

    CHECK(result.state == DiagnosticOrphanRecoveryState::Failed);
    CHECK(result.message.find("before parking") != std::string::npos);
    CHECK(result.previewRestoreAttempted);
    CHECK(result.previewRestoreVerified);
    CHECK_FALSE(result.journalRemoved);
    const auto retained = LoadTunerRecoveryJournal(journal.Get());
    REQUIRE(retained.success);
    REQUIRE(retained.record.has_value());
    CHECK(retained.record->previewRestoreRequired);
    const auto operations = DecodeWireOperations(transport);
    const auto selectorWrites = std::count_if(
        operations.begin(),
        operations.end(),
        [](const WireOperation& operation) {
            return operation.write
                && operation.address
                    == DiagnosticTestBase + Fw2051ScpSelectorRegister;
        });
    CHECK(selectorWrites == 1U);
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

TEST_CASE("an unexpected v4 source value is parked but never overwritten")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    auto record = MakeRecord();
    record.isolatedModuleBaseAddresses.clear();
    record.sourceRestoreRequired = true;
    record.sourceQuad = 7U;
    record.sourceOriginalConfiguration = 0x0040U;
    record.sourceAppliedConfigurationAvailable = true;
    record.sourceAppliedConfiguration = 0x0043U;
    REQUIRE(SaveTunerRecoveryJournal(record, journal.Get()).success);

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references{0x5000U, 0x9E000001U};
    QueueRecoveryFingerprint(transport, references, record);
    QueueTargetIdentity(
        transport,
        references,
        record,
        record.mdppHardwareId);
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
        DiagnosticTestBase + SampleConfigurationRegister,
        0x0042U);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        0U);

    const std::atomic<bool> cancelled{false};
    const auto result = RecoverDiagnosticOrphan(
        transport, {record, journal.Get()}, cancelled);

    CHECK(result.state == DiagnosticOrphanRecoveryState::Failed);
    CHECK_FALSE(result.sourceRestoreAttempted);
    CHECK(result.message.find("live waveform source value")
          != std::string::npos);
    CHECK(result.message.find("refused to overwrite") != std::string::npos);
    CHECK(std::filesystem::exists(journal.Get()));
    const auto operations = DecodeWireOperations(transport);
    CHECK(std::none_of(
        operations.begin(),
        operations.end(),
        [](const WireOperation& operation) {
            return operation.write
                && operation.address
                    == DiagnosticTestBase + SampleConfigurationRegister;
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
