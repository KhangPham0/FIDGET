#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "hardware/DiagnosticSourceOperation.h"
#include "diagnostic_tune_test_support.h"

#include <algorithm>
#include <atomic>
#include <cstdint>

namespace {

constexpr std::uint16_t SampleConfigurationRegister = 0x614AU;

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
    const auto result = ChangeDiagnosticWaveformSource(
        transport, session, {1U, 2U}, cancelled);

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
    const auto result = ChangeDiagnosticWaveformSource(
        transport, session, {1U, 2U}, cancelled);

    CHECK(result.state == DiagnosticSourceChangeState::Failed);
    CHECK(result.writeAttempted);
    CHECK_FALSE(result.writeVerified);
    CHECK(result.rollbackAttempted);
    CHECK(result.rollbackVerified);
    CHECK(result.restoredReadback == 0x00C1U);
    CHECK(result.acquisitionResumed);
    CHECK(result.daqModeResumed);

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

TEST_CASE("invalid source requests issue no wire traffic")
{
    using namespace fidget;
    using namespace fidget::test;

    FakeCommandTransport transport;
    Open(transport);
    auto session = MakeRunningDiagnosticSession();
    const std::atomic<bool> cancelled{false};

    const auto badSource = ChangeDiagnosticWaveformSource(
        transport, session, {1U, 4U}, cancelled);
    const auto badQuad = ChangeDiagnosticWaveformSource(
        transport, session, {8U, 0U}, cancelled);

    CHECK(badSource.state == DiagnosticSourceChangeState::Failed);
    CHECK(badQuad.state == DiagnosticSourceChangeState::Failed);
    CHECK(transport.SentRequests().empty());
}
