#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/RecoveryJournal.h"
#include "hardware/DiagnosticPreviewOperation.h"
#include "diagnostic_tune_test_support.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace {

class JournalPath
{
public:
    JournalPath()
    {
        const auto unique = std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count();
        path_ = (std::filesystem::temp_directory_path()
                 / ("fidget-preview-" + std::to_string(unique)
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

bool ContainsVmeWrite(
    const std::vector<std::byte>& request,
    const std::uint32_t address,
    const std::uint16_t value)
{
    const auto words = fidget::test::DecodeWords(request);
    for (std::size_t index = 0U; index + 4U < words.size(); ++index)
    {
        if (words[index] == fidget::MvlcVmeWriteA32D16Command
            && words[index + 2U] == address
            && static_cast<std::uint16_t>(words[index + 4U]) == value)
        {
            return true;
        }
    }
    return false;
}

void QueuePreviewApply(
    fidget::test::FakeCommandTransport& transport,
    const fidget::DiagnosticAcquisitionPreparationResult& session,
    fidget::test::TransactionReferences& references,
    const std::uint16_t registerOffset,
    const std::uint16_t originalValue,
    const std::uint16_t requestedValue,
    const std::uint16_t readbackValue,
    const bool queueRollback)
{
    using namespace fidget;
    using namespace fidget::test;

    QueueDiagnosticFingerprint(
        transport, session, session.nextSuperReference);
    QueueDiagnosticPause(transport, references);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        7U);
    QueueRead(
        transport,
        references,
        DiagnosticTestBase + registerOffset,
        originalValue);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + registerOffset,
        requestedValue);
    QueueRead(
        transport,
        references,
        DiagnosticTestBase + registerOffset,
        readbackValue);
    if (queueRollback)
    {
        QueueWrite(
            transport,
            references,
            DiagnosticTestBase + registerOffset,
            originalValue);
        QueueRead(
            transport,
            references,
            DiagnosticTestBase + registerOffset,
            originalValue);
    }
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        0U);
    QueueDiagnosticResume(transport, references);
}

} // namespace

TEST_CASE("preview journals the restore value before the parameter write")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    FakeCommandTransport transport;
    Open(transport);
    auto session = MakeRunningDiagnosticSession();
    TransactionReferences references{
        static_cast<std::uint16_t>(session.nextSuperReference + 1U),
        session.nextStackReference,
    };
    QueuePreviewApply(
        transport, session, references, 0x611AU, 200U, 250U, 250U, false);

    bool journalWasArmedBeforeWrite = false;
    transport.SetSendHook([&](const std::vector<std::byte>& request) {
        if (!ContainsVmeWrite(
                request,
                DiagnosticTestBase + 0x611AU,
                250U))
        {
            return;
        }
        const auto loaded = LoadTunerRecoveryJournal(journal.Get());
        journalWasArmedBeforeWrite = loaded.success
            && loaded.record.has_value()
            && loaded.record->previewRestoreRequired
            && loaded.record->previewOriginalValue == 200U
            && loaded.record->previewAppliedValue == 250U;
    });

    const std::atomic<bool> cancelled{false};
    const auto applied = ApplyDiagnosticParameterPreview(
        transport,
        session,
        {7U, 0x611AU, 250U},
        journal.Get(),
        cancelled);

    CHECK(applied.state == DiagnosticParameterPreviewState::PreviewActive);
    CHECK(applied.previewActive);
    CHECK(applied.originalValue == 200U);
    CHECK(applied.appliedReadback == 250U);
    CHECK(applied.applyDurationMicroseconds >= 20U);
    CHECK(journalWasArmedBeforeWrite);
    const auto loaded = LoadTunerRecoveryJournal(journal.Get());
    REQUIRE(loaded.success);
    REQUIRE(loaded.record.has_value());
    CHECK(loaded.record->previewRestoreRequired);
    CHECK(loaded.record->previewQuad == 7U);
    CHECK(loaded.record->previewRegisterOffset == 0x611AU);

    QueueDiagnosticFingerprint(
        transport, session, session.nextSuperReference);
    TransactionReferences restoreReferences{
        static_cast<std::uint16_t>(session.nextSuperReference + 1U),
        session.nextStackReference,
    };
    QueueDiagnosticPause(transport, restoreReferences);
    QueueWrite(
        transport,
        restoreReferences,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        7U);
    QueueRead(
        transport,
        restoreReferences,
        DiagnosticTestBase + 0x611AU,
        250U);
    QueueWrite(
        transport,
        restoreReferences,
        DiagnosticTestBase + 0x611AU,
        200U);
    QueueRead(
        transport,
        restoreReferences,
        DiagnosticTestBase + 0x611AU,
        200U);
    QueueWrite(
        transport,
        restoreReferences,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        0U);
    QueueDiagnosticResume(transport, restoreReferences);

    const auto restored = RestoreDiagnosticParameterPreview(
        transport,
        session,
        applied,
        journal.Get(),
        true,
        false,
        cancelled);
    CHECK(restored.state == DiagnosticParameterPreviewState::Restored);
    CHECK_FALSE(restored.previewActive);
    CHECK(restored.restoreVerified);
    CHECK(restored.restoredReadback == 200U);
    CHECK(restored.restoreDurationMicroseconds >= 20U);
    const auto safeJournal = LoadTunerRecoveryJournal(journal.Get());
    REQUIRE(safeJournal.success);
    REQUIRE(safeJournal.record.has_value());
    CHECK_FALSE(safeJournal.record->previewRestoreRequired);
}

TEST_CASE("preview readback mismatch rolls back and clears the journal")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    FakeCommandTransport transport;
    Open(transport);
    auto session = MakeRunningDiagnosticSession();
    TransactionReferences references{
        static_cast<std::uint16_t>(session.nextSuperReference + 1U),
        session.nextStackReference,
    };
    QueuePreviewApply(
        transport, session, references, 0x611AU, 200U, 250U, 251U, true);

    const std::atomic<bool> cancelled{false};
    const auto result = ApplyDiagnosticParameterPreview(
        transport,
        session,
        {7U, 0x611AU, 250U},
        journal.Get(),
        cancelled);

    CHECK(result.state == DiagnosticParameterPreviewState::Failed);
    CHECK(result.writeAttempted);
    CHECK_FALSE(result.writeVerified);
    CHECK(result.rollbackAttempted);
    CHECK(result.rollbackVerified);
    CHECK_FALSE(result.previewActive);
    CHECK(result.acquisitionResumed);
    CHECK(result.daqModeResumed);
    const auto loaded = LoadTunerRecoveryJournal(journal.Get());
    REQUIRE(loaded.success);
    REQUIRE(loaded.record.has_value());
    CHECK_FALSE(loaded.record->previewRestoreRequired);
}

TEST_CASE("automatic stop restore leaves acquisition paused for cleanup")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    FakeCommandTransport transport;
    Open(transport);
    auto session = MakeRunningDiagnosticSession();
    TransactionReferences references{
        static_cast<std::uint16_t>(session.nextSuperReference + 1U),
        session.nextStackReference,
    };
    QueuePreviewApply(
        transport, session, references, 0x611AU, 200U, 250U, 250U, false);
    const std::atomic<bool> cancelled{false};
    const auto applied = ApplyDiagnosticParameterPreview(
        transport,
        session,
        {7U, 0x611AU, 250U},
        journal.Get(),
        cancelled);
    REQUIRE(applied.previewActive);

    QueueDiagnosticFingerprint(
        transport, session, session.nextSuperReference);
    TransactionReferences restoreReferences{
        static_cast<std::uint16_t>(session.nextSuperReference + 1U),
        session.nextStackReference,
    };
    QueueDiagnosticPause(transport, restoreReferences);
    QueueWrite(
        transport,
        restoreReferences,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        7U);
    QueueRead(
        transport,
        restoreReferences,
        DiagnosticTestBase + 0x611AU,
        250U);
    QueueWrite(
        transport,
        restoreReferences,
        DiagnosticTestBase + 0x611AU,
        200U);
    QueueRead(
        transport,
        restoreReferences,
        DiagnosticTestBase + 0x611AU,
        200U);
    QueueWrite(
        transport,
        restoreReferences,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        0U);

    const auto requestsBeforeRestore = transport.SentRequests().size();
    const auto restored = RestoreDiagnosticParameterPreview(
        transport,
        session,
        applied,
        journal.Get(),
        false,
        true,
        cancelled);

    CHECK(restored.state == DiagnosticParameterPreviewState::Restored);
    CHECK(restored.automaticallyRestoredOnStop);
    CHECK(restored.restoreVerified);
    CHECK_FALSE(restored.acquisitionResumed);
    CHECK_FALSE(restored.daqModeResumed);
    CHECK(transport.SentRequests().size() - requestsBeforeRestore == 15U);
}

TEST_CASE("live dependency violation sends no parameter write")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
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
        7U);
    QueueRead(
        transport,
        references,
        DiagnosticTestBase + 0x6148U,
        400U);
    QueueRead(
        transport,
        references,
        DiagnosticTestBase + 0x6146U,
        400U);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + Fw2051ScpSelectorRegister,
        0U);
    QueueDiagnosticResume(transport, references);

    const std::atomic<bool> cancelled{false};
    const auto result = ApplyDiagnosticParameterPreview(
        transport,
        session,
        {7U, 0x6148U, 398U},
        journal.Get(),
        cancelled);

    CHECK(result.state == DiagnosticParameterPreviewState::Failed);
    CHECK(result.dependencyChecked);
    CHECK(result.dependencyValue == 400U);
    CHECK_FALSE(result.writeAttempted);
    CHECK(result.acquisitionResumed);
    const auto operations = DecodeWireOperations(transport);
    CHECK(std::none_of(
        operations.begin(),
        operations.end(),
        [](const WireOperation& operation) {
            return operation.write
                && operation.address == DiagnosticTestBase + 0x6148U;
        }));
}

TEST_CASE("invalid preview values issue no wire traffic")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    FakeCommandTransport transport;
    Open(transport);
    auto session = MakeRunningDiagnosticSession();
    const std::atomic<bool> cancelled{false};

    const auto result = ApplyDiagnosticParameterPreview(
        transport,
        session,
        {7U, 0x6148U, 399U},
        journal.Get(),
        cancelled);

    CHECK(result.state == DiagnosticParameterPreviewState::Failed);
    CHECK(transport.SentRequests().empty());
}
