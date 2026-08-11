#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/ScpConfiguration.h"
#include "core/StartupPreparation.h"
#include "core/VmeProtocol.h"
#include "fake_command_transport.h"
#include "hardware/StartupPreparationOperation.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

constexpr std::uint32_t Base = 0x11000000U;

struct TransactionReferences
{
    std::uint16_t super = 0x1700U;
    std::uint32_t stack = 0x9C0C0001U;
};

struct WireOperation
{
    bool write = false;
    std::uint32_t address = 0U;
    std::uint16_t value = 0U;
};

std::vector<std::byte> EncodeWords(const std::vector<std::uint32_t>& words)
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

std::vector<std::uint32_t> MakeSuperFrame(const std::uint16_t reference)
{
    return {
        (static_cast<std::uint32_t>(fidget::MvlcSuperFrameType) << 24U) |
            1U,
        fidget::MvlcReferenceWordCommand | reference,
    };
}

std::vector<std::uint32_t> MakeReadStackFrame(
    const std::uint32_t stackReference,
    const std::uint16_t value)
{
    return {
        (static_cast<std::uint32_t>(fidget::MvlcStackFrameType) << 24U) |
            2U,
        stackReference,
        value,
    };
}

std::vector<std::uint32_t> MakeWriteStackFrame(
    const std::uint32_t stackReference)
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
    const std::uint32_t address,
    const std::uint16_t value)
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
    const std::uint32_t address,
    const std::uint16_t value)
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

fidget::StartupAuditResult MakeAudit()
{
    fidget::StartupAuditResult audit;
    audit.state = fidget::StartupAuditState::Complete;
    audit.baseAddress = Base;
    audit.hardwareId = fidget::Mdpp32HardwareId;
    audit.firmwareRevision = fidget::Mdpp32ScpFirmwareRevisionFw2051;
    return audit;
}

std::array<
    std::uint16_t,
    fidget::Fw2051StartupPreparationRegisterCount>
TargetValues()
{
    std::array<
        std::uint16_t,
        fidget::Fw2051StartupPreparationRegisterCount> values{};
    for (std::size_t index = 0U; index < values.size(); ++index)
    {
        values[index] =
            fidget::Fw2051StartupPreparationRegisterTable[index].targetValue;
    }
    return values;
}

void QueuePreparationCapture(
    fidget::test::FakeCommandTransport& transport,
    TransactionReferences& references,
    const std::array<
        std::uint16_t,
        fidget::Fw2051StartupPreparationRegisterCount>& values,
    const std::uint16_t hardwareId = fidget::Mdpp32HardwareId,
    const std::uint16_t firmware =
        fidget::Mdpp32ScpFirmwareRevisionFw2051,
    const std::uint16_t acquisition = 1U)
{
    QueueRead(transport, references, Base + 0x6008U, hardwareId);
    QueueRead(transport, references, Base + 0x600EU, firmware);
    for (std::size_t index = 0U; index < values.size(); ++index)
    {
        QueueRead(
            transport,
            references,
            Base + fidget::Fw2051StartupPreparationRegisterTable[index]
                       .registerOffset,
            values[index]);
    }
    QueueRead(
        transport,
        references,
        Base + fidget::Fw2051AcquisitionControlRegister,
        acquisition);
}

void QueueStop(
    fidget::test::FakeCommandTransport& transport,
    TransactionReferences& references)
{
    QueueWrite(
        transport,
        references,
        Base + fidget::Fw2051AcquisitionControlRegister,
        fidget::Fw2051StopAcquisitionValue);
    QueueRead(
        transport,
        references,
        Base + fidget::Fw2051AcquisitionControlRegister,
        fidget::Fw2051StopAcquisitionValue);
}

void QueueFinalVerification(
    fidget::test::FakeCommandTransport& transport,
    TransactionReferences& references)
{
    QueueRead(
        transport,
        references,
        Base + fidget::Fw2051AcquisitionControlRegister,
        0U);
    for (const auto& definition :
         fidget::Fw2051StartupPreparationRegisterTable)
    {
        QueueRead(
            transport,
            references,
            Base + definition.registerOffset,
            definition.targetValue);
    }
    QueueRead(
        transport,
        references,
        Base + fidget::Fw2051AcquisitionControlRegister,
        0U);
}

std::vector<std::uint32_t> DecodeWords(const std::vector<std::byte>& bytes)
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

TEST_CASE("startup preparation writes only mismatches and proves stopped state")
{
    using namespace fidget;
    using namespace fidget::test;

    auto original = TargetValues();
    original[1] = 2U;
    original[4] = 3U;
    original[7] = 0x0008U;

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references;
    QueuePreparationCapture(transport, references, original);
    QueueStop(transport, references);
    for (const auto index : {1U, 4U, 7U})
    {
        const auto& definition = Fw2051StartupPreparationRegisterTable[index];
        QueueWrite(
            transport,
            references,
            Base + definition.registerOffset,
            definition.targetValue);
        QueueRead(
            transport,
            references,
            Base + definition.registerOffset,
            definition.targetValue);
    }
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
    QueueFinalVerification(transport, references);

    std::vector<std::string> gateNames;
    const std::atomic<bool> cancelled{false};
    const auto result = PrepareFw2051ModuleForStartup(
        transport,
        Base,
        MakeAudit(),
        cancelled,
        AllowAllGates(&gateNames));

    INFO(result.message);
    CHECK(result.state == StartupPreparationState::Passed);
    CHECK(result.settingsRead == 8U);
    CHECK(result.originalAcquisitionValue == 1U);
    CHECK(result.changedSettings == 3U);
    CHECK(result.writesAttempted == 3U);
    CHECK(result.writesVerified == 3U);
    CHECK(result.moduleStopVerified);
    CHECK(result.fifoResetSent);
    CHECK(result.readoutResetSent);
    CHECK(result.moduleLeftStopped);
    CHECK_FALSE(result.rollbackAttempted);
    CHECK(gateNames.size() == 8U);

    const auto operations = DecodeWireOperations(transport);
    REQUIRE(operations.size() == 31U);
    CHECK_FALSE(operations[0].write);
    CHECK(operations[0].address == Base + 0x6008U);
    CHECK_FALSE(operations[10].write);
    CHECK(operations[10].address == Base + Fw2051AcquisitionControlRegister);
    CHECK(operations[11].write);
    CHECK(operations[11].address == Base + Fw2051AcquisitionControlRegister);
    CHECK(operations[13].address == Base + 0x6010U);
    CHECK(operations[15].address == Base + 0x601CU);
    CHECK(operations[17].address == Base + 0x6044U);
    CHECK(operations[19].address == Base + Fw2051FifoResetRegister);
    CHECK(operations[20].address == Base + Fw2051ReadoutResetRegister);
    CHECK_FALSE(operations.back().write);
    CHECK(operations.back().address ==
          Base + Fw2051AcquisitionControlRegister);
}

TEST_CASE(
    "startup preparation rolls settings back in reverse and stays stopped")
{
    using namespace fidget;
    using namespace fidget::test;

    auto original = TargetValues();
    original[1] = 2U;
    original[4] = 3U;

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references;
    QueuePreparationCapture(
        transport, references, original, 0x5007U, 0x2051U, 1U);
    QueueStop(transport, references);
    QueueWrite(transport, references, Base + 0x6010U, 1U);
    QueueRead(transport, references, Base + 0x6010U, 1U);
    QueueWrite(transport, references, Base + 0x601CU, 0U);
    QueueRead(transport, references, Base + 0x601CU, 9U);
    QueueWrite(transport, references, Base + 0x601CU, 3U);
    QueueRead(transport, references, Base + 0x601CU, 3U);
    QueueWrite(transport, references, Base + 0x6010U, 2U);
    QueueRead(transport, references, Base + 0x6010U, 2U);
    QueueRead(
        transport,
        references,
        Base + Fw2051AcquisitionControlRegister,
        0U);

    const std::atomic<bool> cancelled{false};
    const auto result = PrepareFw2051ModuleForStartup(
        transport,
        Base,
        MakeAudit(),
        cancelled,
        AllowAllGates());

    CHECK(result.state == StartupPreparationState::Failed);
    CHECK(result.rollbackAttempted);
    CHECK(result.rollbackVerified);
    CHECK(result.rollbackWritesAttempted == 2U);
    CHECK(result.rollbackWritesVerified == 2U);
    CHECK(result.moduleLeftStopped);

    const auto operations = DecodeWireOperations(transport);
    REQUIRE(operations.size() == 22U);
    CHECK(operations[17].write);
    CHECK(operations[17].address == Base + 0x601CU);
    CHECK(operations[17].value == 3U);
    CHECK(operations[19].write);
    CHECK(operations[19].address == Base + 0x6010U);
    CHECK(operations[19].value == 2U);
    for (const auto& operation : operations)
    {
        if (operation.write &&
            operation.address == Base + Fw2051AcquisitionControlRegister)
        {
            CHECK(operation.value == Fw2051StopAcquisitionValue);
        }
    }
}

TEST_CASE("startup preparation passively stops after foreign takeover")
{
    using namespace fidget;
    using namespace fidget::test;

    auto original = TargetValues();
    original[1] = 2U;
    original[4] = 3U;

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references;
    QueuePreparationCapture(transport, references, original);
    QueueStop(transport, references);
    QueueWrite(transport, references, Base + 0x6010U, 1U);
    QueueRead(transport, references, Base + 0x6010U, 1U);

    const auto gate = [](const std::string& operationName) {
        if (operationName == "module startup preparation IRQ source write")
        {
            return ScpCaptureGateResult{
                ScpCaptureGateStatus::OwnershipLost,
                "A foreign DAQ took ownership."};
        }
        return ScpCaptureGateResult{ScpCaptureGateStatus::Allowed, {}};
    };
    const std::atomic<bool> cancelled{false};
    const auto result = PrepareFw2051ModuleForStartup(
        transport, Base, MakeAudit(), cancelled, gate);

    CHECK(result.state == StartupPreparationState::Failed);
    CHECK(result.message == "A foreign DAQ took ownership.");
    CHECK(result.writesVerified == 1U);
    CHECK(result.rollbackAttempted);
    CHECK(result.rollbackWritesAttempted == 0U);
    CHECK_FALSE(result.rollbackVerified);
    CHECK(DecodeWireOperations(transport).size() == 15U);
}

TEST_CASE("startup preparation rejects an unsuitable audit before wire traffic")
{
    using namespace fidget;
    using namespace fidget::test;

    auto audit = MakeAudit();
    audit.firmwareRevision = 0x2052U;
    FakeCommandTransport transport;
    Open(transport);
    const std::atomic<bool> cancelled{false};
    const auto result = PrepareFw2051ModuleForStartup(
        transport, Base, audit, cancelled, AllowAllGates());

    CHECK(result.state == StartupPreparationState::Failed);
    CHECK(result.message.find("No VME write was sent") != std::string::npos);
    CHECK(transport.SentRequests().empty());
}

TEST_CASE("startup preparation rechecks firmware before the first write")
{
    using namespace fidget;
    using namespace fidget::test;

    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references;
    QueueRead(transport, references, Base + 0x6008U, Mdpp32HardwareId);
    QueueRead(transport, references, Base + 0x600EU, 0x2052U);

    const std::atomic<bool> cancelled{false};
    const auto result = PrepareFw2051ModuleForStartup(
        transport,
        Base,
        MakeAudit(),
        cancelled,
        AllowAllGates());

    CHECK(result.state == StartupPreparationState::Failed);
    CHECK_FALSE(result.strictFirmwareAccepted);
    CHECK(result.writesAttempted == 0U);
    const auto operations = DecodeWireOperations(transport);
    REQUIRE(operations.size() == 2U);
    CHECK_FALSE(operations[0].write);
    CHECK_FALSE(operations[1].write);
}
