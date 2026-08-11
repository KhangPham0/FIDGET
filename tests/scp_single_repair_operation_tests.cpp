#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/ScpConfiguration.h"
#include "core/VmeProtocol.h"
#include "fake_command_transport.h"
#include "hardware/ScpSingleRepairOperation.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

struct TransactionReferences
{
    std::uint16_t super = 0x3A00U;
    std::uint32_t stack = 0x9D100001U;
};

std::vector<std::byte> EncodeWords(
    const std::vector<std::uint32_t>& words)
{
    return fidget::EncodeMvlcWordsLittleEndian(words.data(), words.size());
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
    return EncodeWords(words);
}

std::vector<std::uint32_t> MakeSuperFrame(std::uint16_t reference)
{
    return {
        (static_cast<std::uint32_t>(fidget::MvlcSuperFrameType) << 24U) |
            1U,
        fidget::MvlcReferenceWordCommand | reference,
    };
}

std::vector<std::uint32_t> MakeReadStackFrame(
    std::uint32_t stackReference,
    std::uint16_t value)
{
    return {
        (static_cast<std::uint32_t>(fidget::MvlcStackFrameType) << 24U) |
            2U,
        stackReference,
        value,
    };
}

std::vector<std::uint32_t> MakeWriteStackFrame(
    std::uint32_t stackReference)
{
    return {
        (static_cast<std::uint32_t>(fidget::MvlcStackFrameType) << 24U) |
            1U,
        stackReference,
    };
}

void QueueRead(
    fidget::test::FakeCommandTransport& transport,
    TransactionReferences& references,
    std::uint32_t address,
    std::uint16_t value)
{
    const auto operation = fidget::EncodeMvlcVmeReadD16Words(address);
    const auto upload = fidget::BuildMvlcStackUploadRequest(
        references.super,
        references.stack,
        operation.data(),
        operation.size());
    transport.QueueExchange({
        EncodeWords(upload),
        {fidget::test::FakeReceiveAction::Datagram(
            MakeCommandPacket({MakeSuperFrame(references.super)}))},
    });
    ++references.super;
    transport.QueueExchange({
        EncodeWords(fidget::BuildMvlcStackExecuteRequest(references.super)),
        {fidget::test::FakeReceiveAction::Datagram(MakeCommandPacket({
            MakeSuperFrame(references.super),
            MakeReadStackFrame(references.stack, value),
        }))},
    });
    ++references.super;
    ++references.stack;
}

void QueueWrite(
    fidget::test::FakeCommandTransport& transport,
    TransactionReferences& references,
    std::uint32_t address,
    std::uint16_t value)
{
    const auto operation = fidget::EncodeMvlcVmeWriteD16Words(address, value);
    const auto upload = fidget::BuildMvlcStackUploadRequest(
        references.super,
        references.stack,
        operation.data(),
        operation.size());
    transport.QueueExchange({
        EncodeWords(upload),
        {fidget::test::FakeReceiveAction::Datagram(
            MakeCommandPacket({MakeSuperFrame(references.super)}))},
    });
    ++references.super;
    transport.QueueExchange({
        EncodeWords(fidget::BuildMvlcStackExecuteRequest(references.super)),
        {fidget::test::FakeReceiveAction::Datagram(MakeCommandPacket({
            MakeSuperFrame(references.super),
            MakeWriteStackFrame(references.stack),
        }))},
    });
    ++references.super;
    ++references.stack;
}

void QueueIdentity(
    fidget::test::FakeCommandTransport& transport,
    TransactionReferences& references)
{
    constexpr std::uint32_t Base = 0x11000000U;
    QueueRead(
        transport, references, Base + 0x6008U, fidget::Mdpp32HardwareId);
    QueueRead(
        transport,
        references,
        Base + 0x600EU,
        fidget::Mdpp32ScpFirmwareRevisionFw2051);
}

fidget::ScpCaptureOwnershipGate AllowAllGates(
    std::vector<std::string>* names = nullptr)
{
    return [names](const std::string& name) {
        if (names != nullptr)
        {
            names->push_back(name);
        }
        return fidget::ScpCaptureGateResult{
            fidget::ScpCaptureGateStatus::Allowed, {}};
    };
}

void Open(fidget::test::FakeCommandTransport& transport)
{
    REQUIRE(transport.Open("mvlc-test", 32768U).success);
}

} // namespace

TEST_CASE("single repair retains gain and per-channel threshold values")
{
    using namespace fidget;
    using namespace fidget::test;

    constexpr std::uint32_t Base = 0x11000000U;
    const std::vector<ScpSingleRepairRequest> requests{
        {Base, 7U, 0x611AU, 200U, 250U},
        {Base, 7U, 0x6122U, 2500U, 2600U},
    };

    for (const auto& request : requests)
    {
        FakeCommandTransport transport;
        Open(transport);
        TransactionReferences references;
        QueueIdentity(transport, references);
        QueueWrite(
            transport,
            references,
            Base + Fw2051ScpSelectorRegister,
            request.quad);
        QueueRead(
            transport,
            references,
            Base + request.registerOffset,
            request.expectedLiveValue);
        QueueWrite(
            transport,
            references,
            Base + request.registerOffset,
            request.profileValue);
        QueueRead(
            transport,
            references,
            Base + request.registerOffset,
            request.profileValue);
        QueueWrite(
            transport,
            references,
            Base + Fw2051ScpSelectorRegister,
            0U);

        std::vector<std::string> gateNames;
        const std::atomic<bool> cancelled{false};
        const auto result = RepairFw2051ScpProfileValue(
            transport,
            request,
            cancelled,
            AllowAllGates(&gateNames));

        CHECK(result.state == ScpSingleRepairState::Passed);
        CHECK(result.capturedLiveValue == request.expectedLiveValue);
        CHECK(result.appliedReadback == request.profileValue);
        CHECK(result.writeAttempted);
        CHECK(result.writeVerified);
        CHECK_FALSE(result.rollbackAttempted);
        CHECK(result.profileValueRetained);
        CHECK(result.selectorParkedAtQuadZero);
        CHECK_FALSE(result.dependencyChecked);
        CHECK(gateNames.size() == 4U);
        CHECK(transport.SentRequests().size() == 14U);
    }
}

TEST_CASE("single repair checks the live coupled dependency on the wire")
{
    using namespace fidget;
    using namespace fidget::test;

    constexpr std::uint32_t Base = 0x11000000U;
    const ScpSingleRepairRequest request{
        Base, 7U, 0x6148U, 400U, 398U};
    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references;
    QueueIdentity(transport, references);
    QueueWrite(
        transport, references, Base + Fw2051ScpSelectorRegister, 7U);
    QueueRead(transport, references, Base + 0x6146U, 50U);
    QueueRead(transport, references, Base + 0x6148U, 400U);
    QueueWrite(transport, references, Base + 0x6148U, 398U);
    QueueRead(transport, references, Base + 0x6148U, 398U);
    QueueWrite(
        transport, references, Base + Fw2051ScpSelectorRegister, 0U);

    const std::atomic<bool> cancelled{false};
    const auto result = RepairFw2051ScpProfileValue(
        transport, request, cancelled, AllowAllGates());

    CHECK(result.state == ScpSingleRepairState::Passed);
    CHECK(result.settingName == "Total samples");
    CHECK(result.dependencyChecked);
    CHECK(result.dependencyName == "Pre-samples");
    CHECK(result.dependencyValue == 50U);
    CHECK(result.profileValueRetained);
    CHECK(transport.SentRequests().size() == 16U);
}

TEST_CASE("single repair rejects stale and invalid values before writing")
{
    using namespace fidget;
    using namespace fidget::test;

    constexpr std::uint32_t Base = 0x11000000U;
    SUBCASE("stale live value")
    {
        FakeCommandTransport transport;
        Open(transport);
        TransactionReferences references;
        QueueIdentity(transport, references);
        QueueWrite(
            transport, references, Base + Fw2051ScpSelectorRegister, 7U);
        QueueRead(transport, references, Base + 0x611AU, 201U);
        QueueWrite(
            transport, references, Base + Fw2051ScpSelectorRegister, 0U);

        const std::atomic<bool> cancelled{false};
        const auto result = RepairFw2051ScpProfileValue(
            transport,
            {Base, 7U, 0x611AU, 200U, 250U},
            cancelled,
            AllowAllGates());

        CHECK(result.state == ScpSingleRepairState::Failed);
        CHECK(result.liveValueCaptured);
        CHECK_FALSE(result.writeAttempted);
        CHECK(result.selectorParkedAtQuadZero);
        CHECK(result.message.find("changed after the snapshot") !=
              std::string::npos);
        CHECK(transport.SentRequests().size() == 10U);
    }

    SUBCASE("dependency violation")
    {
        FakeCommandTransport transport;
        Open(transport);
        TransactionReferences references;
        QueueIdentity(transport, references);
        QueueWrite(
            transport, references, Base + Fw2051ScpSelectorRegister, 2U);
        QueueRead(transport, references, Base + 0x6124U, 100U);
        QueueWrite(
            transport, references, Base + Fw2051ScpSelectorRegister, 0U);

        const std::atomic<bool> cancelled{false};
        const auto result = RepairFw2051ScpProfileValue(
            transport,
            {Base, 2U, 0x6110U, 8U, 125U},
            cancelled,
            AllowAllGates());

        CHECK(result.state == ScpSingleRepairState::Failed);
        CHECK(result.dependencyChecked);
        CHECK_FALSE(result.writeAttempted);
        CHECK(result.selectorParkedAtQuadZero);
        CHECK(transport.SentRequests().size() == 10U);
    }

    SUBCASE("registry rejection has no wire traffic")
    {
        FakeCommandTransport transport;
        Open(transport);
        const std::atomic<bool> cancelled{false};
        const auto result = RepairFw2051ScpProfileValue(
            transport,
            {Base, 7U, 0x6148U, 400U, 3U},
            cancelled,
            AllowAllGates());

        CHECK(result.state == ScpSingleRepairState::Failed);
        CHECK(result.message.find("even number of samples") !=
              std::string::npos);
        CHECK(transport.SentRequests().empty());
    }
}

TEST_CASE("single repair rolls back a mismatched readback exactly")
{
    using namespace fidget;
    using namespace fidget::test;

    constexpr std::uint32_t Base = 0x11000000U;
    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references;
    QueueIdentity(transport, references);
    QueueWrite(
        transport, references, Base + Fw2051ScpSelectorRegister, 7U);
    QueueRead(transport, references, Base + 0x611AU, 200U);
    QueueWrite(transport, references, Base + 0x611AU, 250U);
    QueueRead(transport, references, Base + 0x611AU, 249U);
    QueueWrite(transport, references, Base + 0x611AU, 200U);
    QueueRead(transport, references, Base + 0x611AU, 200U);
    QueueWrite(
        transport, references, Base + Fw2051ScpSelectorRegister, 0U);

    const std::atomic<bool> cancelled{false};
    const auto result = RepairFw2051ScpProfileValue(
        transport,
        {Base, 7U, 0x611AU, 200U, 250U},
        cancelled,
        AllowAllGates());

    CHECK(result.state == ScpSingleRepairState::Failed);
    CHECK(result.writeAttempted);
    CHECK_FALSE(result.writeVerified);
    CHECK(result.rollbackAttempted);
    CHECK(result.rollbackVerified);
    CHECK(result.rollbackReadback == 200U);
    CHECK_FALSE(result.profileValueRetained);
    CHECK(result.selectorParkedAtQuadZero);
    CHECK(transport.SentRequests().size() == 18U);
}

TEST_CASE("single repair passively stops after foreign ownership appears")
{
    using namespace fidget;
    using namespace fidget::test;

    constexpr std::uint32_t Base = 0x11000000U;
    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references;
    QueueIdentity(transport, references);
    QueueWrite(
        transport, references, Base + Fw2051ScpSelectorRegister, 7U);
    QueueRead(transport, references, Base + 0x611AU, 200U);

    const std::atomic<bool> cancelled{false};
    const auto gate = [](const std::string& operationName) {
        if (operationName == "the SCP profile write")
        {
            return ScpCaptureGateResult{
                ScpCaptureGateStatus::OwnershipLost,
                "A foreign DAQ took ownership."};
        }
        return ScpCaptureGateResult{ScpCaptureGateStatus::Allowed, {}};
    };
    const auto result = RepairFw2051ScpProfileValue(
        transport,
        {Base, 7U, 0x611AU, 200U, 250U},
        cancelled,
        gate);

    CHECK(result.state == ScpSingleRepairState::Failed);
    CHECK_FALSE(result.writeAttempted);
    CHECK_FALSE(result.selectorParkedAtQuadZero);
    CHECK(result.message == "A foreign DAQ took ownership.");
    CHECK(transport.SentRequests().size() == 8U);
}
