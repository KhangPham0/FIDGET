#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/ScpConfiguration.h"
#include "core/VmeProtocol.h"
#include "fake_command_transport.h"
#include "hardware/ScpSingleRepairOperation.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t Base = 0x11000000U;

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
    TransactionReferences& references,
    const std::uint16_t hardwareId = fidget::Mdpp32HardwareId)
{
    QueueRead(
        transport, references, Base + 0x6008U, hardwareId);
    QueueRead(
        transport,
        references,
        Base + 0x600EU,
        fidget::Mdpp32ScpFirmwareRevisionFw2051);
}

struct WireOperation
{
    bool write = false;
    std::uint32_t address = 0U;
    std::uint16_t value = 0U;
};

std::vector<std::uint32_t> DecodeWords(
    const std::vector<std::byte>& bytes)
{
    REQUIRE(bytes.size() % sizeof(std::uint32_t) == 0U);
    std::vector<std::uint32_t> words;
    for (std::size_t offset = 0U;
         offset < bytes.size();
         offset += sizeof(std::uint32_t))
    {
        words.push_back(fidget::LoadLittleEndian32(bytes.data() + offset));
    }
    return words;
}

std::vector<WireOperation> DecodeWireOperations(
    const fidget::test::FakeCommandTransport& transport)
{
    std::vector<WireOperation> operations;
    for (const auto& request : transport.SentRequests())
    {
        const auto words = DecodeWords(request);
        for (std::size_t index = 0U; index < words.size(); ++index)
        {
            if (words[index] == fidget::MvlcVmeReadA32D16Command)
            {
                REQUIRE(index + 2U < words.size());
                operations.push_back({false, words[index + 2U], 0U});
            }
            else if (words[index] == fidget::MvlcVmeWriteA32D16Command)
            {
                REQUIRE(index + 4U < words.size());
                operations.push_back({
                    true,
                    words[index + 2U],
                    static_cast<std::uint16_t>(words[index + 4U]),
                });
            }
        }
    }
    return operations;
}

void CheckWireOperations(
    const std::vector<WireOperation>& actual,
    const std::vector<WireOperation>& expected)
{
    REQUIRE(actual.size() == expected.size());
    for (std::size_t index = 0U; index < expected.size(); ++index)
    {
        CHECK(actual[index].write == expected[index].write);
        CHECK(actual[index].address == expected[index].address);
        CHECK(actual[index].value == expected[index].value);
    }
}

void QueueRunningGainRollback(
    fidget::test::FakeCommandTransport& transport,
    TransactionReferences& references,
    const std::uint16_t rollbackReadback = 200U)
{
    using namespace fidget;

    QueueIdentity(transport, references);
    QueueRead(
        transport,
        references,
        Base + Fw2051AcquisitionControlRegister,
        1U);
    QueueWrite(
        transport,
        references,
        Base + Fw2051AcquisitionControlRegister,
        Fw2051StopAcquisitionValue);
    QueueRead(
        transport,
        references,
        Base + Fw2051AcquisitionControlRegister,
        Fw2051StopAcquisitionValue);
    QueueWrite(
        transport, references, Base + Fw2051ScpSelectorRegister, 7U);
    QueueRead(transport, references, Base + 0x611AU, 200U);
    QueueWrite(transport, references, Base + 0x611AU, 250U);
    QueueRead(transport, references, Base + 0x611AU, 249U);
    QueueWrite(transport, references, Base + 0x611AU, 200U);
    QueueRead(
        transport,
        references,
        Base + 0x611AU,
        rollbackReadback);
    QueueWrite(
        transport, references, Base + Fw2051ScpSelectorRegister, 0U);
    QueueWrite(
        transport,
        references,
        Base + Fw2051FifoResetRegister,
        Fw2051ResetCommandValue);
    QueueWrite(
        transport,
        references,
        Base + Fw2051ReadoutResetRegister,
        Fw2051ResetCommandValue);
    QueueRead(
        transport,
        references,
        Base + Fw2051AcquisitionControlRegister,
        Fw2051StopAcquisitionValue);
}

std::vector<WireOperation> RunningGainRollbackOperations()
{
    using namespace fidget;

    return {
        {false, Base + 0x6008U, 0U},
        {false, Base + 0x600EU, 0U},
        {false, Base + Fw2051AcquisitionControlRegister, 0U},
        {true,
         Base + Fw2051AcquisitionControlRegister,
         Fw2051StopAcquisitionValue},
        {false, Base + Fw2051AcquisitionControlRegister, 0U},
        {true, Base + Fw2051ScpSelectorRegister, 7U},
        {false, Base + 0x611AU, 0U},
        {true, Base + 0x611AU, 250U},
        {false, Base + 0x611AU, 0U},
        {true, Base + 0x611AU, 200U},
        {false, Base + 0x611AU, 0U},
        {true, Base + Fw2051ScpSelectorRegister, 0U},
        {true,
         Base + Fw2051FifoResetRegister,
         Fw2051ResetCommandValue},
        {true,
         Base + Fw2051ReadoutResetRegister,
         Fw2051ResetCommandValue},
        {false, Base + Fw2051AcquisitionControlRegister, 0U},
    };
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

TEST_CASE("single repair does not convert read-only MDPP-32 v2 recognition into write authority")
{
    using namespace fidget;
    using namespace fidget::test;

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references;
    QueueIdentity(transport, references, Mdpp32AlternateHardwareId);
    const std::atomic<bool> cancelled{false};
    const auto result = RepairFw2051ScpProfileValue(
        transport,
        {Base, 7U, 0x611AU, 200U, 250U},
        cancelled,
        AllowAllGates());

    INFO(result.message);
    CHECK(result.state == ScpSingleRepairState::Failed);
    CHECK(result.message.find("await recorded hardware acceptance")
          != std::string::npos);
    CHECK_FALSE(result.writeAttempted);
    const auto operations = DecodeWireOperations(transport);
    REQUIRE(operations.size() == 1U);
    CHECK_FALSE(operations.front().write);
}

TEST_CASE("single repair stops a running module and retains profile values")
{
    using namespace fidget;
    using namespace fidget::test;

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
        QueueRead(
            transport,
            references,
            Base + Fw2051AcquisitionControlRegister,
            1U);
        QueueWrite(
            transport,
            references,
            Base + Fw2051AcquisitionControlRegister,
            Fw2051StopAcquisitionValue);
        QueueRead(
            transport,
            references,
            Base + Fw2051AcquisitionControlRegister,
            Fw2051StopAcquisitionValue);
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
        QueueWrite(
            transport,
            references,
            Base + Fw2051FifoResetRegister,
            Fw2051ResetCommandValue);
        QueueWrite(
            transport,
            references,
            Base + Fw2051ReadoutResetRegister,
            Fw2051ResetCommandValue);
        QueueRead(
            transport,
            references,
            Base + Fw2051AcquisitionControlRegister,
            Fw2051StopAcquisitionValue);

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
        CHECK(result.moduleStopSent);
        CHECK(result.moduleStopVerified);
        CHECK(result.profileValueRetained);
        CHECK(result.selectorParkedAtQuadZero);
        CHECK(result.fifoResetSent);
        CHECK(result.readoutResetSent);
        CHECK(result.moduleLeftStopped);
        CHECK_FALSE(result.dependencyChecked);
        CHECK(gateNames.size() == 8U);
        CHECK(transport.SentRequests().size() == 26U);
        CHECK(result.message.find("module remains stopped") !=
              std::string::npos);
    }
}

TEST_CASE("single repair accepts an already-stopped module without stopping again")
{
    using namespace fidget;
    using namespace fidget::test;

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references;
    QueueIdentity(transport, references);
    QueueRead(
        transport,
        references,
        Base + Fw2051AcquisitionControlRegister,
        Fw2051StopAcquisitionValue);
    QueueWrite(
        transport, references, Base + Fw2051ScpSelectorRegister, 7U);
    QueueRead(transport, references, Base + 0x611AU, 200U);
    QueueWrite(transport, references, Base + 0x611AU, 250U);
    QueueRead(transport, references, Base + 0x611AU, 250U);
    QueueWrite(
        transport, references, Base + Fw2051ScpSelectorRegister, 0U);
    QueueWrite(
        transport,
        references,
        Base + Fw2051FifoResetRegister,
        Fw2051ResetCommandValue);
    QueueWrite(
        transport,
        references,
        Base + Fw2051ReadoutResetRegister,
        Fw2051ResetCommandValue);
    QueueRead(
        transport,
        references,
        Base + Fw2051AcquisitionControlRegister,
        Fw2051StopAcquisitionValue);

    const std::atomic<bool> cancelled{false};
    const auto result = RepairFw2051ScpProfileValue(
        transport,
        {Base, 7U, 0x611AU, 200U, 250U},
        cancelled,
        AllowAllGates());

    CHECK(result.state == ScpSingleRepairState::Passed);
    CHECK_FALSE(result.moduleStopSent);
    CHECK(result.moduleStopVerified);
    CHECK(result.fifoResetSent);
    CHECK(result.readoutResetSent);
    CHECK(result.moduleLeftStopped);
    CHECK(transport.SentRequests().size() == 22U);
    const auto operations = DecodeWireOperations(transport);
    CHECK(std::none_of(
        operations.begin(),
        operations.end(),
        [](const WireOperation& operation) {
            return operation.write && operation.address ==
                Base + fidget::Fw2051AcquisitionControlRegister;
        }));
}

TEST_CASE("single repair requires a final stopped-state proof")
{
    using namespace fidget;
    using namespace fidget::test;

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references;
    QueueIdentity(transport, references);
    QueueRead(
        transport,
        references,
        Base + Fw2051AcquisitionControlRegister,
        Fw2051StopAcquisitionValue);
    QueueWrite(
        transport, references, Base + Fw2051ScpSelectorRegister, 7U);
    QueueRead(transport, references, Base + 0x611AU, 200U);
    QueueWrite(transport, references, Base + 0x611AU, 250U);
    QueueRead(transport, references, Base + 0x611AU, 250U);
    QueueWrite(
        transport, references, Base + Fw2051ScpSelectorRegister, 0U);
    QueueWrite(
        transport,
        references,
        Base + Fw2051FifoResetRegister,
        Fw2051ResetCommandValue);
    QueueWrite(
        transport,
        references,
        Base + Fw2051ReadoutResetRegister,
        Fw2051ResetCommandValue);
    QueueRead(
        transport,
        references,
        Base + Fw2051AcquisitionControlRegister,
        1U);

    const std::atomic<bool> cancelled{false};
    const auto result = RepairFw2051ScpProfileValue(
        transport,
        {Base, 7U, 0x611AU, 200U, 250U},
        cancelled,
        AllowAllGates());

    CHECK(result.state == ScpSingleRepairState::Failed);
    CHECK(result.moduleStopVerified);
    CHECK(result.writeVerified);
    CHECK(result.profileValueRetained);
    CHECK(result.selectorParkedAtQuadZero);
    CHECK(result.fifoResetSent);
    CHECK(result.readoutResetSent);
    CHECK_FALSE(result.moduleLeftStopped);
    CHECK(result.message.find("did not remain stopped") !=
          std::string::npos);
    CHECK(result.message.find("module remains stopped") ==
          std::string::npos);
}

TEST_CASE("single repair checks the live coupled dependency on the wire")
{
    using namespace fidget;
    using namespace fidget::test;

    const ScpSingleRepairRequest request{
        Base, 7U, 0x6148U, 400U, 398U};
    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references;
    QueueIdentity(transport, references);
    QueueRead(
        transport,
        references,
        Base + Fw2051AcquisitionControlRegister,
        Fw2051StopAcquisitionValue);
    QueueWrite(
        transport, references, Base + Fw2051ScpSelectorRegister, 7U);
    QueueRead(transport, references, Base + 0x6146U, 50U);
    QueueRead(transport, references, Base + 0x6148U, 400U);
    QueueWrite(transport, references, Base + 0x6148U, 398U);
    QueueRead(transport, references, Base + 0x6148U, 398U);
    QueueWrite(
        transport, references, Base + Fw2051ScpSelectorRegister, 0U);
    QueueWrite(
        transport,
        references,
        Base + Fw2051FifoResetRegister,
        Fw2051ResetCommandValue);
    QueueWrite(
        transport,
        references,
        Base + Fw2051ReadoutResetRegister,
        Fw2051ResetCommandValue);
    QueueRead(
        transport,
        references,
        Base + Fw2051AcquisitionControlRegister,
        Fw2051StopAcquisitionValue);

    const std::atomic<bool> cancelled{false};
    const auto result = RepairFw2051ScpProfileValue(
        transport, request, cancelled, AllowAllGates());

    CHECK(result.state == ScpSingleRepairState::Passed);
    CHECK(result.settingName == "Total samples");
    CHECK(result.dependencyChecked);
    CHECK(result.dependencyName == "Pre-samples");
    CHECK(result.dependencyValue == 50U);
    CHECK(result.profileValueRetained);
    CHECK(result.moduleLeftStopped);
    CHECK(transport.SentRequests().size() == 24U);
}

TEST_CASE("single repair rejects stale and invalid values before writing")
{
    using namespace fidget;
    using namespace fidget::test;

    SUBCASE("stale live value")
    {
        FakeCommandTransport transport;
        Open(transport);
        TransactionReferences references;
        QueueIdentity(transport, references);
        QueueRead(
            transport,
            references,
            Base + Fw2051AcquisitionControlRegister,
            Fw2051StopAcquisitionValue);
        QueueWrite(
            transport, references, Base + Fw2051ScpSelectorRegister, 7U);
        QueueRead(transport, references, Base + 0x611AU, 201U);
        QueueWrite(
            transport, references, Base + Fw2051ScpSelectorRegister, 0U);
        QueueRead(
            transport,
            references,
            Base + Fw2051AcquisitionControlRegister,
            Fw2051StopAcquisitionValue);

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
        CHECK_FALSE(result.fifoResetSent);
        CHECK_FALSE(result.readoutResetSent);
        CHECK(result.moduleLeftStopped);
        CHECK(result.message.find("changed after the snapshot") !=
              std::string::npos);
        CHECK(transport.SentRequests().size() == 14U);
    }

    SUBCASE("dependency violation")
    {
        FakeCommandTransport transport;
        Open(transport);
        TransactionReferences references;
        QueueIdentity(transport, references);
        QueueRead(
            transport,
            references,
            Base + Fw2051AcquisitionControlRegister,
            Fw2051StopAcquisitionValue);
        QueueWrite(
            transport, references, Base + Fw2051ScpSelectorRegister, 2U);
        QueueRead(transport, references, Base + 0x6124U, 100U);
        QueueWrite(
            transport, references, Base + Fw2051ScpSelectorRegister, 0U);
        QueueRead(
            transport,
            references,
            Base + Fw2051AcquisitionControlRegister,
            Fw2051StopAcquisitionValue);

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
        CHECK_FALSE(result.fifoResetSent);
        CHECK_FALSE(result.readoutResetSent);
        CHECK(result.moduleLeftStopped);
        CHECK(transport.SentRequests().size() == 14U);
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

TEST_CASE("single repair sends no further write when stop verification fails")
{
    using namespace fidget;
    using namespace fidget::test;

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references;
    QueueIdentity(transport, references);
    QueueRead(
        transport,
        references,
        Base + Fw2051AcquisitionControlRegister,
        1U);
    QueueWrite(
        transport,
        references,
        Base + Fw2051AcquisitionControlRegister,
        Fw2051StopAcquisitionValue);
    QueueRead(
        transport,
        references,
        Base + Fw2051AcquisitionControlRegister,
        1U);

    const std::atomic<bool> cancelled{false};
    const auto result = RepairFw2051ScpProfileValue(
        transport,
        {Base, 7U, 0x611AU, 200U, 250U},
        cancelled,
        AllowAllGates());

    CHECK(result.state == ScpSingleRepairState::Failed);
    CHECK(result.moduleStopSent);
    CHECK_FALSE(result.moduleStopVerified);
    CHECK_FALSE(result.writeAttempted);
    CHECK_FALSE(result.selectorParkedAtQuadZero);
    CHECK_FALSE(result.fifoResetSent);
    CHECK_FALSE(result.readoutResetSent);
    CHECK_FALSE(result.moduleLeftStopped);
    CHECK(result.message.find("expected zero after Stop") !=
          std::string::npos);
    CheckWireOperations(
        DecodeWireOperations(transport),
        {
            {false, Base + 0x6008U, 0U},
            {false, Base + 0x600EU, 0U},
            {false, Base + Fw2051AcquisitionControlRegister, 0U},
            {true,
             Base + Fw2051AcquisitionControlRegister,
             Fw2051StopAcquisitionValue},
            {false, Base + Fw2051AcquisitionControlRegister, 0U},
        });
}

TEST_CASE("single repair settles every frontend write including rollback")
{
    using namespace fidget;
    using namespace fidget::test;

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references;
    QueueRunningGainRollback(transport, references);

    std::vector<std::pair<std::int64_t, std::size_t>> settleCalls;
    const std::atomic<bool> cancelled{false};
    const auto result = RepairFw2051ScpProfileValue(
        transport,
        {Base, 7U, 0x611AU, 200U, 250U},
        cancelled,
        AllowAllGates(),
        [&](const std::chrono::microseconds duration) {
            settleCalls.emplace_back(
                duration.count(), transport.SentRequests().size());
        });

    CHECK(result.state == ScpSingleRepairState::Failed);
    CHECK(result.writeAttempted);
    CHECK_FALSE(result.writeVerified);
    CHECK(result.rollbackAttempted);
    CHECK(result.rollbackVerified);
    CHECK(result.rollbackReadback == 200U);
    CHECK_FALSE(result.profileValueRetained);
    CHECK(result.moduleStopSent);
    CHECK(result.moduleStopVerified);
    CHECK(result.selectorParkedAtQuadZero);
    CHECK(result.fifoResetSent);
    CHECK(result.readoutResetSent);
    CHECK(result.moduleLeftStopped);
    CHECK(transport.SentRequests().size() == 30U);
    const std::vector<std::pair<std::int64_t, std::size_t>> expectedSettles{
        {50, 12U},
        {20, 16U},
        {20, 20U},
        {50, 24U},
    };
    CHECK(settleCalls == expectedSettles);
    CheckWireOperations(
        DecodeWireOperations(transport),
        RunningGainRollbackOperations());
}

TEST_CASE("single repair cleans up and remains stopped after rollback mismatch")
{
    using namespace fidget;
    using namespace fidget::test;

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references;
    QueueRunningGainRollback(transport, references, 201U);

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
    CHECK_FALSE(result.rollbackVerified);
    CHECK(result.rollbackReadback == 201U);
    CHECK_FALSE(result.profileValueRetained);
    CHECK(result.selectorParkedAtQuadZero);
    CHECK(result.fifoResetSent);
    CHECK(result.readoutResetSent);
    CHECK(result.moduleLeftStopped);
    CHECK(result.message.find("rollback readback was 201") !=
          std::string::npos);
    CHECK(result.message.find("restored with exact readback") ==
          std::string::npos);
    CheckWireOperations(
        DecodeWireOperations(transport),
        RunningGainRollbackOperations());
}

TEST_CASE("single repair sends no blind write after ownership becomes uncertain")
{
    using namespace fidget;
    using namespace fidget::test;

    struct GateBoundary
    {
        const char* name;
        std::size_t completedOperations;
    };
    const std::vector<GateBoundary> boundaries{
        {"the SCP profile module stop", 3U},
        {"the SCP profile bank selection", 5U},
        {"the SCP profile write", 7U},
        {"the SCP profile rollback", 9U},
        {"the SCP profile selector parking", 11U},
        {"the SCP profile FIFO reset", 12U},
        {"the SCP profile readout reset", 13U},
        {"the SCP profile final stopped-state verification", 14U},
    };
    const std::vector<ScpCaptureGateStatus> deniedStatuses{
        ScpCaptureGateStatus::CommunicationUnavailable,
        ScpCaptureGateStatus::OwnershipLost,
    };
    const auto fullOperations = RunningGainRollbackOperations();

    for (const auto status : deniedStatuses)
    {
        for (const auto& boundary : boundaries)
        {
            CAPTURE(boundary.name);
            CAPTURE(status);
            FakeCommandTransport transport;
            Open(transport);
            TransactionReferences references;
            QueueRunningGainRollback(transport, references);

            const std::string denialMessage = status ==
                    ScpCaptureGateStatus::CommunicationUnavailable
                ? "Communication is uncertain at " +
                    std::string(boundary.name) + "."
                : "Foreign ownership was proven at " +
                    std::string(boundary.name) + ".";
            const auto gate = [&](const std::string& operationName) {
                if (operationName == boundary.name)
                {
                    return ScpCaptureGateResult{
                        status,
                        denialMessage,
                    };
                }
                return ScpCaptureGateResult{
                    ScpCaptureGateStatus::Allowed,
                    {},
                };
            };

            const std::atomic<bool> cancelled{false};
            const auto result = RepairFw2051ScpProfileValue(
                transport,
                {Base, 7U, 0x611AU, 200U, 250U},
                cancelled,
                gate);

            CHECK(result.state == ScpSingleRepairState::Failed);
            CHECK(result.message.find(denialMessage) != std::string::npos);
            CHECK(result.communicationUnavailable ==
                  (status ==
                   ScpCaptureGateStatus::CommunicationUnavailable));
            CHECK_FALSE(result.moduleLeftStopped);

            auto expected = fullOperations;
            expected.resize(boundary.completedOperations);
            CheckWireOperations(
                DecodeWireOperations(transport), expected);
            CHECK(transport.SentRequests().size() ==
                  boundary.completedOperations * 2U);
        }
    }
}
