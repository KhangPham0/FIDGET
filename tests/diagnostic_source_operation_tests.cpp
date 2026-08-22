#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "hardware/DiagnosticSourceOperation.h"
#include "hardware/DiagnosticPreviewOperation.h"
#include "diagnostic_tune_test_support.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace {

constexpr std::uint16_t SampleConfigurationRegister = 0x614AU;

std::string MakeJournalPath()
{
    const char* temporaryDirectory = std::getenv("TMPDIR");
    if (temporaryDirectory == nullptr || temporaryDirectory[0] == '\0')
    {
        temporaryDirectory = "/tmp";
    }
    const auto unique = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    return std::string(temporaryDirectory)
        + "/fidget-source-operation-" + std::to_string(unique)
        + ".recovery";
}

} // namespace

TEST_CASE("source change preserves every non-source configuration bit")
{
    using namespace fidget;
    using namespace fidget::test;

    FakeCommandTransport transport;
    Open(transport);
    auto session = MakeRunningDiagnosticSession();
    QueueDiagnosticFingerprint(
        transport, session, session.nextSuperReference);
    TransactionReferences references{
        static_cast<std::uint16_t>(session.nextSuperReference + 1U),
        session.nextStackReference,
    };
    QueueDiagnosticPause(transport, references);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        1U);
    QueueRead(
        transport,
        references,
        DiagnosticTestBase + SampleConfigurationRegister,
        0x00C1U);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + SampleConfigurationRegister,
        0x00C2U);
    QueueRead(
        transport,
        references,
        DiagnosticTestBase + SampleConfigurationRegister,
        0x00C2U);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        0U);
    QueueDiagnosticResume(transport, references, 0x00000005U);

    const std::atomic<bool> cancelled{false};
    const auto journalPath = MakeJournalPath();
    const auto result = ChangeDiagnosticWaveformSource(
        transport, session, {1U, 2U}, journalPath, cancelled);

    CHECK(result.state == DiagnosticSourceChangeState::Passed);
    CHECK(result.fingerprintVerified);
    CHECK(result.originalConfiguration == 0x00C1U);
    CHECK(result.requestedConfiguration == 0x00C2U);
    CHECK(result.appliedReadback == 0x00C2U);
    CHECK(result.writeVerified);
    CHECK(result.selectorParkedAtQuadZero);
    CHECK(result.acquisitionResumed);
    CHECK(result.daqModeReadback == 0x00000005U);
    CHECK(result.daqModeResumed);
    CHECK(result.sourceRestoreRequired);
    CHECK(session.recoveryRecord.sourceRestoreRequired);
    CHECK(session.recoveryRecord.sourceQuad == 1U);
    CHECK(session.recoveryRecord.sourceOriginalConfiguration == 0x00C1U);
    CHECK(session.recoveryRecord.sourceAppliedConfigurationAvailable);
    CHECK(session.recoveryRecord.sourceAppliedConfiguration == 0x00C2U);

    const auto loaded = LoadTunerRecoveryJournal(journalPath);
    REQUIRE(loaded.success);
    REQUIRE(loaded.record.has_value());
    CHECK(loaded.record->sourceAppliedConfigurationAvailable);
    CHECK(loaded.record->sourceAppliedConfiguration == 0x00C2U);

    std::string removeError;
    CHECK(RemoveTunerRecoveryJournal(journalPath, removeError));

    const auto operations = DecodeWireOperations(transport);
    const auto sourceWrite = std::find_if(
        operations.begin(),
        operations.end(),
        [](const WireOperation& operation) {
            return operation.write
                && operation.address
                    == DiagnosticTestBase + SampleConfigurationRegister;
        });
    REQUIRE(sourceWrite != operations.end());
    CHECK(sourceWrite->value == 0x00C2U);
}

TEST_CASE("source readback mismatch restores the exact captured word")
{
    using namespace fidget;
    using namespace fidget::test;

    FakeCommandTransport transport;
    Open(transport);
    auto session = MakeRunningDiagnosticSession();
    QueueDiagnosticFingerprint(
        transport, session, session.nextSuperReference);
    TransactionReferences references{
        static_cast<std::uint16_t>(session.nextSuperReference + 1U),
        session.nextStackReference,
    };
    QueueDiagnosticPause(transport, references);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        1U);
    QueueRead(
        transport,
        references,
        DiagnosticTestBase + SampleConfigurationRegister,
        0x00C1U);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + SampleConfigurationRegister,
        0x00C2U);
    QueueRead(
        transport,
        references,
        DiagnosticTestBase + SampleConfigurationRegister,
        0x00C3U);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + SampleConfigurationRegister,
        0x00C1U);
    QueueRead(
        transport,
        references,
        DiagnosticTestBase + SampleConfigurationRegister,
        0x00C1U);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        0U);
    QueueDiagnosticResume(transport, references);

    const std::atomic<bool> cancelled{false};
    const auto journalPath = MakeJournalPath();
    const auto result = ChangeDiagnosticWaveformSource(
        transport, session, {1U, 2U}, journalPath, cancelled);

    CHECK(result.state == DiagnosticSourceChangeState::Failed);
    CHECK(result.writeAttempted);
    CHECK_FALSE(result.writeVerified);
    CHECK(result.rollbackAttempted);
    CHECK(result.rollbackVerified);
    CHECK(result.restoredReadback == 0x00C1U);
    CHECK(result.acquisitionResumed);
    CHECK(result.daqModeResumed);
    CHECK_FALSE(result.sourceRestoreRequired);
    CHECK_FALSE(session.recoveryRecord.sourceRestoreRequired);
    CHECK_FALSE(
        session.recoveryRecord.sourceAppliedConfigurationAvailable);
    CHECK(session.recoveryRecord.sourceAppliedConfiguration == 0U);

    std::string removeError;
    CHECK(RemoveTunerRecoveryJournal(journalPath, removeError));

    const auto operations = DecodeWireOperations(transport);
    std::vector<std::uint16_t> sourceWrites;
    for (const auto& operation : operations)
    {
        if (operation.write
            && operation.address
                == DiagnosticTestBase + SampleConfigurationRegister)
        {
            sourceWrites.push_back(operation.value);
        }
    }
    CHECK(sourceWrites == std::vector<std::uint16_t>{0x00C2U, 0x00C1U});
}

TEST_CASE("another temporary source requires restoring the original first")
{
    using namespace fidget;
    using namespace fidget::test;

    FakeCommandTransport transport;
    Open(transport);
    auto session = MakeRunningDiagnosticSession();
    session.recoveryRecord.sourceRestoreRequired = true;
    session.recoveryRecord.sourceQuad = 1U;
    session.recoveryRecord.sourceOriginalConfiguration = 0x00C1U;
    session.recoveryRecord.sourceAppliedConfigurationAvailable = true;
    session.recoveryRecord.sourceAppliedConfiguration = 0x00C2U;
    const auto journalPath = MakeJournalPath();
    REQUIRE(SaveTunerRecoveryJournal(
        session.recoveryRecord, journalPath).success);

    const std::atomic<bool> cancelled{false};
    const auto result = ChangeDiagnosticWaveformSource(
        transport, session, {1U, 3U}, journalPath, cancelled);

    CHECK(result.state == DiagnosticSourceChangeState::Failed);
    CHECK_FALSE(result.writeAttempted);
    CHECK(result.sourceRestoreRequired);
    CHECK(result.message.find("Restore the original waveform source")
          != std::string::npos);
    CHECK(transport.SentRequests().empty());
    CHECK(session.recoveryRecord.sourceOriginalConfiguration == 0x00C1U);
    CHECK(session.recoveryRecord.sourceAppliedConfigurationAvailable);
    CHECK(session.recoveryRecord.sourceAppliedConfiguration == 0x00C2U);
    const auto loaded = LoadTunerRecoveryJournal(journalPath);
    REQUIRE(loaded.success);
    REQUIRE(loaded.record.has_value());
    CHECK(loaded.record->sourceOriginalConfiguration == 0x00C1U);
    CHECK(loaded.record->sourceAppliedConfigurationAvailable);
    CHECK(loaded.record->sourceAppliedConfiguration == 0x00C2U);

    std::string removeError;
    CHECK(RemoveTunerRecoveryJournal(journalPath, removeError));
}

TEST_CASE("invalid source requests issue no wire traffic")
{
    using namespace fidget;
    using namespace fidget::test;

    FakeCommandTransport transport;
    Open(transport);
    auto session = MakeRunningDiagnosticSession();
    const std::atomic<bool> cancelled{false};

    const auto badSource = ChangeDiagnosticWaveformSource(
        transport, session, {1U, 4U}, MakeJournalPath(), cancelled);
    const auto badQuad = ChangeDiagnosticWaveformSource(
        transport, session, {8U, 0U}, MakeJournalPath(), cancelled);

    CHECK(badSource.state == DiagnosticSourceChangeState::Failed);
    CHECK(badQuad.state == DiagnosticSourceChangeState::Failed);
    CHECK(transport.SentRequests().empty());
}

TEST_CASE("same-value source changes never arm crash recovery")
{
    using namespace fidget;
    using namespace fidget::test;

    FakeCommandTransport transport;
    Open(transport);
    auto session = MakeRunningDiagnosticSession();
    QueueDiagnosticFingerprint(
        transport, session, session.nextSuperReference);
    TransactionReferences references{
        static_cast<std::uint16_t>(session.nextSuperReference + 1U),
        session.nextStackReference,
    };
    QueueDiagnosticPause(transport, references);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        1U);
    QueueRead(
        transport,
        references,
        DiagnosticTestBase + SampleConfigurationRegister,
        0x00C2U);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        0U);
    QueueDiagnosticResume(transport, references);

    const std::atomic<bool> cancelled{false};
    const auto journalPath = MakeJournalPath();
    const auto result = ChangeDiagnosticWaveformSource(
        transport, session, {1U, 2U}, journalPath, cancelled);

    CHECK(result.state == DiagnosticSourceChangeState::Passed);
    CHECK_FALSE(result.writeAttempted);
    CHECK_FALSE(result.sourceRestoreRequired);
    CHECK_FALSE(session.recoveryRecord.sourceRestoreRequired);
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

TEST_CASE("switching back to the original source clears its recovery state")
{
    using namespace fidget;
    using namespace fidget::test;

    FakeCommandTransport transport;
    Open(transport);
    auto session = MakeRunningDiagnosticSession();
    session.recoveryRecord.sourceRestoreRequired = true;
    session.recoveryRecord.sourceQuad = 1U;
    session.recoveryRecord.sourceOriginalConfiguration = 0x00C1U;
    session.recoveryRecord.sourceAppliedConfigurationAvailable = true;
    session.recoveryRecord.sourceAppliedConfiguration = 0x00C2U;
    const auto journalPath = MakeJournalPath();
    REQUIRE(SaveTunerRecoveryJournal(
        session.recoveryRecord, journalPath).success);
    QueueDiagnosticFingerprint(
        transport, session, session.nextSuperReference);
    TransactionReferences references{
        static_cast<std::uint16_t>(session.nextSuperReference + 1U),
        session.nextStackReference,
    };
    QueueDiagnosticPause(transport, references);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        1U);
    QueueRead(
        transport,
        references,
        DiagnosticTestBase + SampleConfigurationRegister,
        0x00C2U);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + SampleConfigurationRegister,
        0x00C1U);
    QueueRead(
        transport,
        references,
        DiagnosticTestBase + SampleConfigurationRegister,
        0x00C1U);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        0U);
    QueueDiagnosticResume(transport, references);

    const std::atomic<bool> cancelled{false};
    const auto result = ChangeDiagnosticWaveformSource(
        transport, session, {1U, 1U}, journalPath, cancelled);

    CHECK(result.state == DiagnosticSourceChangeState::Passed);
    CHECK(result.writeVerified);
    CHECK_FALSE(result.sourceRestoreRequired);
    CHECK_FALSE(session.recoveryRecord.sourceRestoreRequired);
    const auto loaded = LoadTunerRecoveryJournal(journalPath);
    REQUIRE(loaded.success);
    REQUIRE(loaded.record.has_value());
    CHECK_FALSE(loaded.record->sourceRestoreRequired);
    CHECK_FALSE(loaded.record->sourceAppliedConfigurationAvailable);
    CHECK(loaded.record->sourceAppliedConfiguration == 0U);

    std::string removeError;
    CHECK(RemoveTunerRecoveryJournal(journalPath, removeError));
}

TEST_CASE("source writes are refused when the restore value cannot be journaled")
{
    using namespace fidget;
    using namespace fidget::test;

    FakeCommandTransport transport;
    Open(transport);
    auto session = MakeRunningDiagnosticSession();
    QueueDiagnosticFingerprint(
        transport, session, session.nextSuperReference);
    TransactionReferences references{
        static_cast<std::uint16_t>(session.nextSuperReference + 1U),
        session.nextStackReference,
    };
    QueueDiagnosticPause(transport, references);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        1U);
    QueueRead(
        transport,
        references,
        DiagnosticTestBase + SampleConfigurationRegister,
        0x00C1U);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        0U);
    QueueDiagnosticResume(transport, references);

    const std::atomic<bool> cancelled{false};
    const auto result = ChangeDiagnosticWaveformSource(
        transport,
        session,
        {1U, 2U},
        "/dev/null/fidget-source.recovery",
        cancelled);

    CHECK(result.state == DiagnosticSourceChangeState::Failed);
    CHECK_FALSE(result.writeAttempted);
    CHECK_FALSE(session.recoveryRecord.sourceRestoreRequired);
    CHECK(result.message.find("no source write was allowed")
          != std::string::npos);
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

TEST_CASE("stop-time source restore is exact and clears the journal field")
{
    using namespace fidget;
    using namespace fidget::test;

    FakeCommandTransport transport;
    Open(transport);
    auto session = MakeRunningDiagnosticSession();
    session.recoveryRecord.sourceRestoreRequired = true;
    session.recoveryRecord.sourceQuad = 7U;
    session.recoveryRecord.sourceOriginalConfiguration = 0x0040U;
    session.recoveryRecord.sourceAppliedConfigurationAvailable = true;
    session.recoveryRecord.sourceAppliedConfiguration = 0x0043U;
    const auto journalPath = MakeJournalPath();
    REQUIRE(SaveTunerRecoveryJournal(
        session.recoveryRecord, journalPath).success);

    QueueDiagnosticFingerprint(
        transport, session, session.nextSuperReference);
    TransactionReferences references{
        static_cast<std::uint16_t>(session.nextSuperReference + 1U),
        session.nextStackReference,
    };
    QueueDiagnosticPause(transport, references);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        7U);
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

    const std::atomic<bool> cancelled{false};
    const auto result = RestoreDiagnosticWaveformSource(
        transport, session, journalPath, false, cancelled);

    CHECK(result.state == DiagnosticSourceChangeState::Passed);
    CHECK(result.restoreAttempted);
    CHECK(result.restoreVerified);
    CHECK(result.restoredReadback == 0x0040U);
    CHECK(result.selectorParkedAtQuadZero);
    CHECK_FALSE(result.acquisitionResumed);
    CHECK_FALSE(result.sourceRestoreRequired);
    CHECK_FALSE(session.recoveryRecord.sourceRestoreRequired);
    CHECK_FALSE(
        session.recoveryRecord.sourceAppliedConfigurationAvailable);
    CHECK(session.recoveryRecord.sourceAppliedConfiguration == 0U);
    const auto loaded = LoadTunerRecoveryJournal(journalPath);
    REQUIRE(loaded.success);
    REQUIRE(loaded.record.has_value());
    CHECK_FALSE(loaded.record->sourceRestoreRequired);
    CHECK_FALSE(loaded.record->sourceAppliedConfigurationAvailable);
    CHECK(loaded.record->sourceAppliedConfiguration == 0U);

    std::string removeError;
    CHECK(RemoveTunerRecoveryJournal(journalPath, removeError));
}

TEST_CASE("failed stop-time source restore retains recovery evidence")
{
    using namespace fidget;
    using namespace fidget::test;

    FakeCommandTransport transport;
    Open(transport);
    auto session = MakeRunningDiagnosticSession();
    session.recoveryRecord.sourceRestoreRequired = true;
    session.recoveryRecord.sourceQuad = 7U;
    session.recoveryRecord.sourceOriginalConfiguration = 0x0040U;
    session.recoveryRecord.sourceAppliedConfigurationAvailable = true;
    session.recoveryRecord.sourceAppliedConfiguration = 0x0043U;
    const auto journalPath = MakeJournalPath();
    REQUIRE(SaveTunerRecoveryJournal(
        session.recoveryRecord, journalPath).success);

    QueueDiagnosticFingerprint(
        transport, session, session.nextSuperReference);
    TransactionReferences references{
        static_cast<std::uint16_t>(session.nextSuperReference + 1U),
        session.nextStackReference,
    };
    QueueDiagnosticPause(transport, references);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        7U);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + SampleConfigurationRegister,
        0x0040U);
    QueueRead(
        transport,
        references,
        DiagnosticTestBase + SampleConfigurationRegister,
        0x0043U);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        0U);

    const std::atomic<bool> cancelled{false};
    const auto result = RestoreDiagnosticWaveformSource(
        transport, session, journalPath, false, cancelled);

    CHECK(result.state == DiagnosticSourceChangeState::Failed);
    CHECK(result.restoreAttempted);
    CHECK_FALSE(result.restoreVerified);
    CHECK(result.sourceRestoreRequired);
    CHECK(session.recoveryRecord.sourceRestoreRequired);
    const auto loaded = LoadTunerRecoveryJournal(journalPath);
    REQUIRE(loaded.success);
    REQUIRE(loaded.record.has_value());
    CHECK(loaded.record->sourceRestoreRequired);
    CHECK(loaded.record->sourceAppliedConfigurationAvailable);
    CHECK(loaded.record->sourceAppliedConfiguration == 0x0043U);

    std::string removeError;
    CHECK(RemoveTunerRecoveryJournal(journalPath, removeError));
}

TEST_CASE("stop restores a preview before restoring its source deviation")
{
    using namespace fidget;
    using namespace fidget::test;

    FakeCommandTransport transport;
    Open(transport);
    auto session = MakeRunningDiagnosticSession();
    session.recoveryRecord.previewRestoreRequired = true;
    session.recoveryRecord.previewQuad = 7U;
    session.recoveryRecord.previewRegisterOffset = 0x611AU;
    session.recoveryRecord.previewOriginalValue = 200U;
    session.recoveryRecord.previewAppliedValue = 250U;
    session.recoveryRecord.sourceRestoreRequired = true;
    session.recoveryRecord.sourceQuad = 7U;
    session.recoveryRecord.sourceOriginalConfiguration = 0x0040U;
    session.recoveryRecord.sourceAppliedConfigurationAvailable = true;
    session.recoveryRecord.sourceAppliedConfiguration = 0x0043U;
    const auto journalPath = MakeJournalPath();
    REQUIRE(SaveTunerRecoveryJournal(
        session.recoveryRecord, journalPath).success);

    QueueDiagnosticFingerprint(
        transport, session, session.nextSuperReference);
    TransactionReferences references{
        static_cast<std::uint16_t>(session.nextSuperReference + 1U),
        session.nextStackReference,
    };
    QueueDiagnosticPause(transport, references);
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

    DiagnosticParameterPreviewResult activePreview;
    activePreview.state = DiagnosticParameterPreviewState::PreviewActive;
    activePreview.selectedQuad = 7U;
    activePreview.registerOffset = 0x611AU;
    activePreview.requestedValue = 250U;
    activePreview.originalValue = 200U;
    activePreview.previewActive = true;
    activePreview.writeAttempted = true;
    activePreview.writeVerified = true;
    activePreview.originalCaptured = true;
    activePreview.appliedReadback = 250U;

    const std::atomic<bool> cancelled{false};
    const auto preview = RestoreDiagnosticParameterPreview(
        transport,
        session,
        activePreview,
        journalPath,
        false,
        true,
        cancelled);
    REQUIRE(preview.restoreVerified);
    REQUIRE_FALSE(session.recoveryRecord.previewRestoreRequired);
    REQUIRE(session.recoveryRecord.sourceRestoreRequired);

    const auto source = RestoreDiagnosticWaveformSource(
        transport, session, journalPath, true, cancelled);
    CHECK(source.restoreVerified);
    CHECK_FALSE(session.recoveryRecord.sourceRestoreRequired);

    const auto operations = DecodeWireOperations(transport);
    std::size_t previewRestoreIndex = operations.size();
    std::size_t sourceRestoreIndex = operations.size();
    for (std::size_t index = 0U; index < operations.size(); ++index)
    {
        if (!operations[index].write)
        {
            continue;
        }
        if (operations[index].address == DiagnosticTestBase + 0x611AU
            && operations[index].value == 200U)
        {
            previewRestoreIndex = index;
        }
        if (operations[index].address
                == DiagnosticTestBase + SampleConfigurationRegister
            && operations[index].value == 0x0040U)
        {
            sourceRestoreIndex = index;
        }
    }
    REQUIRE(previewRestoreIndex < operations.size());
    REQUIRE(sourceRestoreIndex < operations.size());
    CHECK(previewRestoreIndex < sourceRestoreIndex);

    std::string removeError;
    CHECK(RemoveTunerRecoveryJournal(journalPath, removeError));
}
