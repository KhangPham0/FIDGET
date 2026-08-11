#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/ScpRegistry.h"
#include "core/VmeProtocol.h"
#include "fake_command_transport.h"
#include "hardware/ScpCaptureOperation.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

struct TransactionReferences
{
    std::uint16_t super = 0x1800U;
    std::uint32_t stack = 0x9C100001U;
};

struct WireOperation
{
    bool write = false;
    std::uint32_t address = 0U;
    std::uint16_t value = 0U;
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
        (static_cast<std::uint32_t>(fidget::MvlcSuperFrameType) << 24U)
            | 1U,
        fidget::MvlcReferenceWordCommand | reference,
    };
}

std::vector<std::uint32_t> MakeReadStackFrame(
    std::uint32_t stackReference,
    std::uint16_t value,
    std::uint8_t flags = 0U)
{
    return {
        (static_cast<std::uint32_t>(fidget::MvlcStackFrameType) << 24U)
            | (static_cast<std::uint32_t>(flags) << 20U) | 2U,
        stackReference,
        value,
    };
}

std::vector<std::uint32_t> MakeWriteStackFrame(
    std::uint32_t stackReference)
{
    return {
        (static_cast<std::uint32_t>(fidget::MvlcStackFrameType) << 24U)
            | 1U,
        stackReference,
    };
}

std::vector<std::byte> MakeExecuteRequest(std::uint16_t superReference)
{
    return EncodeWords(fidget::BuildMvlcStackExecuteRequest(superReference));
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
        MakeExecuteRequest(references.super),
        {fidget::test::FakeReceiveAction::Datagram(
            MakeCommandPacket({
                MakeSuperFrame(references.super),
                MakeReadStackFrame(references.stack, value),
            }))},
    });
    ++references.super;
    ++references.stack;
}

void QueueFailedRead(
    fidget::test::FakeCommandTransport& transport,
    TransactionReferences& references,
    std::uint32_t address)
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
        MakeExecuteRequest(references.super),
        {fidget::test::FakeReceiveAction::Datagram(
            MakeCommandPacket({
                MakeSuperFrame(references.super),
                MakeReadStackFrame(
                    references.stack,
                    0U,
                    fidget::MvlcBusErrorFlag),
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
        MakeExecuteRequest(references.super),
        {fidget::test::FakeReceiveAction::Datagram(
            MakeCommandPacket({
                MakeSuperFrame(references.super),
                MakeWriteStackFrame(references.stack),
            }))},
    });
    ++references.super;
    ++references.stack;
}

std::uint16_t BankValue(std::uint16_t quad, std::size_t settingIndex)
{
    return static_cast<std::uint16_t>(
        1000U + quad * 100U + settingIndex);
}

void QueueGlobals(
    fidget::test::FakeCommandTransport& transport,
    TransactionReferences& references,
    std::uint16_t hardwareId = fidget::Mdpp32HardwareId,
    std::uint16_t firmware = fidget::Mdpp32ScpFirmwareRevisionFw2051)
{
    constexpr std::uint32_t Base = 0x11000000U;
    QueueRead(transport, references, Base + 0x6008U, hardwareId);
    QueueRead(transport, references, Base + 0x600EU, firmware);
    QueueRead(transport, references, Base + 0x6010U, 1U);
    QueueRead(transport, references, Base + 0x6044U, 0x18U);
}

void QueueBank(
    fidget::test::FakeCommandTransport& transport,
    TransactionReferences& references,
    std::uint16_t quad)
{
    constexpr std::uint32_t Base = 0x11000000U;
    QueueWrite(
        transport,
        references,
        Base + fidget::Fw2051ScpSelectorRegister,
        quad);
    for (std::size_t index = 0U;
         index < fidget::Fw2051ScpSettingRegistry.size(); ++index)
    {
        QueueRead(
            transport,
            references,
            Base + fidget::Fw2051ScpSettingRegistry[index].registerOffset,
            BankValue(quad, index));
    }
}

void QueueCompleteCapture(
    fidget::test::FakeCommandTransport& transport)
{
    constexpr std::uint32_t Base = 0x11000000U;
    TransactionReferences references;
    QueueGlobals(transport, references);
    for (std::uint16_t quad = 0U;
         quad < fidget::Fw2051ScpQuadCount;
         ++quad)
    {
        QueueBank(transport, references, quad);
    }
    QueueWrite(
        transport,
        references,
        Base + fidget::Fw2051ScpSelectorRegister,
        0U);
}

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

void Open(fidget::test::FakeCommandTransport& transport)
{
    REQUIRE(transport.Open("mvlc-test", 32768U).success);
}

} // namespace

TEST_CASE("the capture performs 149 ordered operations and nine gates")
{
    using namespace fidget;
    using namespace fidget::test;

    constexpr std::uint32_t Base = 0x11000000U;
    FakeCommandTransport transport;
    Open(transport);
    QueueCompleteCapture(transport);

    constexpr std::array<std::size_t, 9> ExpectedRequestCounts{
        0U, 44U, 80U, 116U, 152U, 188U, 224U, 260U, 296U};
    std::vector<std::string> gateNames;
    const auto gate = [&](const std::string& name) {
        REQUIRE(gateNames.size() < ExpectedRequestCounts.size());
        CHECK(transport.SentRequests().size() ==
              ExpectedRequestCounts[gateNames.size()]);
        gateNames.push_back(name);
        return ScpCaptureGateResult{ScpCaptureGateStatus::Allowed, {}};
    };
    const std::atomic<bool> cancelled{false};
    const auto result = CaptureFw2051ScpConfiguration(
        transport, Base, cancelled, gate);

    REQUIRE(result.configuration.state == ScpConfigurationState::Complete);
    CHECK(result.configuration.hardwareId == Mdpp32HardwareId);
    CHECK(result.configuration.firmwareRevision ==
          Mdpp32ScpFirmwareRevisionFw2051);
    CHECK(result.configuration.irqLevel == 1U);
    CHECK(result.configuration.outputFormat == 0x18U);
    CHECK(result.configuration.quads.size() == Fw2051ScpQuadCount);
    CHECK(result.configuration.selectorParkedAtQuadZero);
    REQUIRE(gateNames.size() == 9U);
    CHECK(gateNames.front() == "the SCP configuration snapshot");
    CHECK(gateNames[1] == "SCP configuration bank 1");
    CHECK(gateNames[7] == "SCP configuration bank 7");
    CHECK(gateNames.back() == "the SCP selector parking write");

    const auto operations = DecodeWireOperations(transport);
    REQUIRE(operations.size() == 149U);
    CHECK(transport.SentRequests().size() == 298U);

    const std::array<std::uint32_t, 4> globalAddresses{
        Base + 0x6008U,
        Base + 0x600EU,
        Base + 0x6010U,
        Base + 0x6044U,
    };
    for (std::size_t index = 0U; index < globalAddresses.size(); ++index)
    {
        CHECK_FALSE(operations[index].write);
        CHECK(operations[index].address == globalAddresses[index]);
    }

    std::size_t operationIndex = globalAddresses.size();
    for (std::uint16_t quad = 0U; quad < Fw2051ScpQuadCount; ++quad)
    {
        REQUIRE(operationIndex < operations.size());
        CHECK(operations[operationIndex].write);
        CHECK(operations[operationIndex].address ==
              Base + Fw2051ScpSelectorRegister);
        CHECK(operations[operationIndex].value == quad);
        ++operationIndex;

        for (const auto& definition : Fw2051ScpSettingRegistry)
        {
            REQUIRE(operationIndex < operations.size());
            CHECK_FALSE(operations[operationIndex].write);
            CHECK(operations[operationIndex].address ==
                  Base + definition.registerOffset);
            ++operationIndex;
        }
    }
    REQUIRE(operationIndex < operations.size());
    CHECK(operations[operationIndex].write);
    CHECK(operations[operationIndex].address ==
          Base + Fw2051ScpSelectorRegister);
    CHECK(operations[operationIndex].value == 0U);
    CHECK(operationIndex + 1U == operations.size());

    for (std::uint16_t quadIndex = 0U;
         quadIndex < Fw2051ScpQuadCount;
         ++quadIndex)
    {
        const auto& quad = result.configuration.quads[quadIndex];
        CHECK(quad.quad == quadIndex);
        for (std::size_t settingIndex = 0U;
             settingIndex < Fw2051ScpSettingRegistry.size();
             ++settingIndex)
        {
            const auto captured = Fw2051ScpQuadRegisterValue(
                quad,
                Fw2051ScpSettingRegistry[settingIndex].registerOffset);
            REQUIRE(captured.has_value());
            CHECK(*captured == BankValue(quadIndex, settingIndex));
        }
    }
}

TEST_CASE("foreign ownership before a later bank prevents selector parking")
{
    using namespace fidget;
    using namespace fidget::test;

    constexpr std::uint32_t Base = 0x11000000U;
    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references;
    QueueGlobals(transport, references);
    QueueBank(transport, references, 0U);
    QueueBank(transport, references, 1U);
    QueueBank(transport, references, 2U);

    std::size_t gateCount = 0U;
    const auto gate = [&](const std::string&) {
        ++gateCount;
        if (gateCount == 4U)
        {
            return ScpCaptureGateResult{
                ScpCaptureGateStatus::OwnershipLost,
                "foreign DAQ",
            };
        }
        return ScpCaptureGateResult{ScpCaptureGateStatus::Allowed, {}};
    };
    const std::atomic<bool> cancelled{false};
    const auto result = CaptureFw2051ScpConfiguration(
        transport, Base, cancelled, gate);

    CHECK(result.configuration.state == ScpConfigurationState::Failed);
    CHECK(result.configuration.message == "foreign DAQ");
    CHECK(result.configuration.quads.size() == 3U);
    CHECK_FALSE(result.configuration.selectorParkedAtQuadZero);
    CHECK(result.lastGateStatus == ScpCaptureGateStatus::OwnershipLost);

    const auto operations = DecodeWireOperations(transport);
    std::vector<std::uint16_t> selectorValues;
    for (const auto& operation : operations)
    {
        if (operation.write &&
            operation.address == Base + Fw2051ScpSelectorRegister)
        {
            selectorValues.push_back(operation.value);
        }
    }
    CHECK(selectorValues == std::vector<std::uint16_t>{0U, 1U, 2U});
}

TEST_CASE("a bank read failure still parks while ownership remains certain")
{
    using namespace fidget;
    using namespace fidget::test;

    constexpr std::uint32_t Base = 0x11000000U;
    FakeCommandTransport transport;
    Open(transport);
    TransactionReferences references;
    QueueGlobals(transport, references);
    QueueBank(transport, references, 0U);
    QueueBank(transport, references, 1U);
    QueueWrite(
        transport,
        references,
        Base + Fw2051ScpSelectorRegister,
        2U);
    for (std::size_t index = 0U; index < 5U; ++index)
    {
        QueueRead(
            transport,
            references,
            Base + Fw2051ScpSettingRegistry[index].registerOffset,
            BankValue(2U, index));
    }
    QueueFailedRead(
        transport,
        references,
        Base + Fw2051ScpSettingRegistry[5].registerOffset);
    QueueWrite(
        transport,
        references,
        Base + Fw2051ScpSelectorRegister,
        0U);

    std::size_t gateCount = 0U;
    const auto gate = [&](const std::string&) {
        ++gateCount;
        return ScpCaptureGateResult{ScpCaptureGateStatus::Allowed, {}};
    };
    const std::atomic<bool> cancelled{false};
    const auto result = CaptureFw2051ScpConfiguration(
        transport, Base, cancelled, gate);

    CHECK(result.configuration.state == ScpConfigurationState::Failed);
    CHECK(result.configuration.message.find("Quad 2") != std::string::npos);
    CHECK(result.configuration.message.find("Gain") != std::string::npos);
    CHECK(result.configuration.message.find("VME bus error") !=
          std::string::npos);
    CHECK(result.configuration.quads.size() == 2U);
    CHECK(result.configuration.selectorParkedAtQuadZero);
    CHECK(gateCount == 4U);

    const auto operations = DecodeWireOperations(transport);
    REQUIRE(operations.size() >= 2U);
    CHECK(operations[operations.size() - 1U].write);
    CHECK(operations[operations.size() - 1U].address ==
          Base + Fw2051ScpSelectorRegister);
    CHECK(operations[operations.size() - 1U].value == 0U);
}

TEST_CASE("unsupported hardware and firmware stop before selector writes")
{
    using namespace fidget;
    using namespace fidget::test;

    struct Rejection
    {
        std::uint16_t hardwareId;
        std::uint16_t firmware;
        const char* messageFragment;
    };
    const std::array<Rejection, 3> rejections{{
        {0x1234U, Mdpp32ScpFirmwareRevisionFw2051, "not an MDPP-32"},
        {Mdpp32HardwareId, 0x1051U, "supported SCP FW2051"},
        {Mdpp32HardwareId, 0x2052U, "supported SCP FW2051"},
    }};

    for (const auto& rejection : rejections)
    {
        CAPTURE(rejection.hardwareId);
        CAPTURE(rejection.firmware);
        FakeCommandTransport transport;
        Open(transport);
        TransactionReferences references;
        QueueGlobals(
            transport,
            references,
            rejection.hardwareId,
            rejection.firmware);

        std::size_t gateCount = 0U;
        const auto gate = [&](const std::string&) {
            ++gateCount;
            return ScpCaptureGateResult{
                ScpCaptureGateStatus::Allowed,
                {},
            };
        };
        const std::atomic<bool> cancelled{false};
        const auto result = CaptureFw2051ScpConfiguration(
            transport, 0x11000000U, cancelled, gate);

        CHECK(result.configuration.state == ScpConfigurationState::Failed);
        CHECK(result.configuration.message.find(rejection.messageFragment)
              != std::string::npos);
        CHECK(result.configuration.quads.empty());
        CHECK_FALSE(result.configuration.selectorParkedAtQuadZero);
        CHECK(gateCount == 1U);
        const auto operations = DecodeWireOperations(transport);
        CHECK(operations.size() == 4U);
        for (const auto& operation : operations)
        {
            CHECK_FALSE(operation.write);
        }
    }
}
