#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/ScpConfiguration.h"
#include "core/ScpRegistry.h"
#include "core/VmeProtocol.h"
#include "fake_command_transport.h"
#include "hardware/ScpBulkApplyOperation.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace {

struct TransactionReferences
{
    std::uint16_t super = 0U;
    std::uint32_t stack = 0U;
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

fidget::Fw2051ScpConfigurationSnapshot MakeConfiguration()
{
    using namespace fidget;

    Fw2051ScpConfigurationSnapshot configuration;
    configuration.state = ScpConfigurationState::Complete;
    configuration.message = "bulk operation test";
    configuration.baseAddress = 0x11000000U;
    configuration.hardwareId = Mdpp32HardwareId;
    configuration.firmwareRevision = Mdpp32ScpFirmwareRevisionFw2051;
    configuration.irqLevel = 1U;
    configuration.outputFormat = 0x18U;
    configuration.selectorParkedAtQuadZero = true;
    for (std::uint16_t quadIndex = 0U;
         quadIndex < Fw2051ScpQuadCount;
         ++quadIndex)
    {
        Fw2051ScpQuadConfiguration quad;
        quad.quad = quadIndex;
        quad.timingFilter = static_cast<std::uint16_t>(10U + quadIndex);
        quad.poleZero = {
            static_cast<std::uint16_t>(2000U + quadIndex * 10U),
            static_cast<std::uint16_t>(2001U + quadIndex * 10U),
            static_cast<std::uint16_t>(2002U + quadIndex * 10U),
            static_cast<std::uint16_t>(2003U + quadIndex * 10U),
        };
        quad.gain = quadIndex == 7U ? 250U : 200U;
        quad.thresholds = {
            static_cast<std::uint16_t>(2500U + quadIndex * 10U),
            static_cast<std::uint16_t>(2501U + quadIndex * 10U),
            static_cast<std::uint16_t>(2502U + quadIndex * 10U),
            static_cast<std::uint16_t>(2503U + quadIndex * 10U),
        };
        quad.shapingTime = static_cast<std::uint16_t>(160U + quadIndex);
        quad.baselineRestorer = 2U;
        quad.resetTime = 16U;
        quad.signalRiseTime = 4U;
        quad.preSamples = 50U;
        quad.totalSamples = 400U;
        quad.sampleConfiguration = quadIndex == 7U ? 3U : 0U;
        configuration.quads.push_back(quad);
    }
    return configuration;
}

fidget::ScpProfileApplicationRequest MakeRequest()
{
    fidget::ScpProfileApplicationRequest request;
    request.expectedLiveConfiguration = MakeConfiguration();
    request.valuesCompared = 141U;
    request.configurationDifferences = 2U;
    request.steps = {
        {7, 0x611AU, "Gain", 250U, 200U, false},
        {7, 0x614AU, "Sample configuration", 3U, 0U, true},
    };
    return request;
}

void QueueCapture(
    fidget::test::FakeCommandTransport& transport,
    const fidget::Fw2051ScpConfigurationSnapshot& configuration,
    bool changeGain = false)
{
    using namespace fidget;

    TransactionReferences references{0x1800U, 0x9C100001U};
    QueueRead(
        transport,
        references,
        configuration.baseAddress + 0x6008U,
        configuration.hardwareId);
    QueueRead(
        transport,
        references,
        configuration.baseAddress + 0x600EU,
        configuration.firmwareRevision);
    QueueRead(
        transport,
        references,
        configuration.baseAddress + 0x6010U,
        configuration.irqLevel);
    QueueRead(
        transport,
        references,
        configuration.baseAddress + 0x6044U,
        configuration.outputFormat);

    for (std::size_t quadIndex = 0U;
         quadIndex < configuration.quads.size();
         ++quadIndex)
    {
        QueueWrite(
            transport,
            references,
            configuration.baseAddress + Fw2051ScpSelectorRegister,
            static_cast<std::uint16_t>(quadIndex));
        for (const auto& definition : Fw2051ScpSettingRegistry)
        {
            const auto value = Fw2051ScpQuadRegisterValue(
                configuration.quads[quadIndex], definition.registerOffset);
            REQUIRE(value.has_value());
            const bool selectedMismatch = changeGain && quadIndex == 7U &&
                definition.registerOffset == 0x611AU;
            QueueRead(
                transport,
                references,
                configuration.baseAddress + definition.registerOffset,
                static_cast<std::uint16_t>(
                    *value + (selectedMismatch ? 1U : 0U)));
        }
    }
    QueueWrite(
        transport,
        references,
        configuration.baseAddress + Fw2051ScpSelectorRegister,
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

TEST_CASE("bulk apply preflights, stops, retains, parks, and resets")
{
    using namespace fidget;
    using namespace fidget::test;

    constexpr std::uint32_t Base = 0x11000000U;
    const auto request = MakeRequest();
    FakeCommandTransport transport;
    Open(transport);
    QueueCapture(transport, request.expectedLiveConfiguration);

    TransactionReferences references{0x3C00U, 0x9D200001U};
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
    QueueWrite(transport, references, Base + 0x611AU, 200U);
    QueueRead(transport, references, Base + 0x611AU, 200U);
    QueueWrite(transport, references, Base + 0x614AU, 0U);
    QueueRead(transport, references, Base + 0x614AU, 0U);
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

    std::vector<std::string> gateNames;
    const std::atomic<bool> cancelled{false};
    const auto result = ApplyFw2051ScpProfile(
        transport, request, cancelled, AllowAllGates(&gateNames));

    CHECK(result.state == ScpBulkApplyState::Passed);
    CHECK(result.fullPreflightMatched);
    CHECK(result.moduleStopSent);
    CHECK(result.moduleStopVerified);
    CHECK(result.moduleLeftStopped);
    CHECK(result.writesAttempted == 2U);
    CHECK(result.writesVerified == 2U);
    CHECK_FALSE(result.rollbackAttempted);
    CHECK(result.profileValuesRetained);
    CHECK(result.selectorParkedAtQuadZero);
    CHECK(result.fifoResetSent);
    CHECK(result.readoutResetSent);
    REQUIRE(result.values.size() == 2U);
    CHECK(result.values[0].profileValueRetained);
    CHECK(result.values[1].profileValueRetained);
    CHECK(gateNames.size() == 15U);

    const auto operations = DecodeWireOperations(transport);
    REQUIRE(operations.size() == 159U);
    CHECK(operations[149U].write);
    CHECK(operations[149U].address ==
          Base + Fw2051AcquisitionControlRegister);
    CHECK_FALSE(operations[150U].write);
    CHECK(operations[151U].address == Base + Fw2051ScpSelectorRegister);
    CHECK(operations[151U].value == 7U);
    CHECK(operations[152U].address == Base + 0x611AU);
    CHECK(operations[154U].address == Base + 0x614AU);
    CHECK(operations[156U].address == Base + Fw2051ScpSelectorRegister);
    CHECK(operations[156U].value == 0U);
    CHECK(operations[157U].address == Base + Fw2051FifoResetRegister);
    CHECK(operations[158U].address == Base + Fw2051ReadoutResetRegister);
}

TEST_CASE("bulk apply rolls back every attempted value in reverse order")
{
    using namespace fidget;
    using namespace fidget::test;

    constexpr std::uint32_t Base = 0x11000000U;
    const auto request = MakeRequest();
    FakeCommandTransport transport;
    Open(transport);
    QueueCapture(transport, request.expectedLiveConfiguration);

    TransactionReferences references{0x3C00U, 0x9D200001U};
    QueueWrite(
        transport, references, Base + Fw2051AcquisitionControlRegister, 0U);
    QueueRead(
        transport, references, Base + Fw2051AcquisitionControlRegister, 0U);
    QueueWrite(
        transport, references, Base + Fw2051ScpSelectorRegister, 7U);
    QueueWrite(transport, references, Base + 0x611AU, 200U);
    QueueRead(transport, references, Base + 0x611AU, 200U);
    QueueWrite(transport, references, Base + 0x614AU, 0U);
    QueueRead(transport, references, Base + 0x614AU, 3U);
    QueueWrite(
        transport, references, Base + Fw2051ScpSelectorRegister, 7U);
    QueueWrite(transport, references, Base + 0x614AU, 3U);
    QueueRead(transport, references, Base + 0x614AU, 3U);
    QueueWrite(transport, references, Base + 0x611AU, 250U);
    QueueRead(transport, references, Base + 0x611AU, 250U);
    QueueWrite(
        transport, references, Base + Fw2051ScpSelectorRegister, 0U);
    QueueWrite(
        transport, references, Base + Fw2051FifoResetRegister, 1U);
    QueueWrite(
        transport, references, Base + Fw2051ReadoutResetRegister, 1U);

    const std::atomic<bool> cancelled{false};
    const auto result = ApplyFw2051ScpProfile(
        transport, request, cancelled, AllowAllGates());

    CHECK(result.state == ScpBulkApplyState::Failed);
    CHECK(result.fullPreflightMatched);
    CHECK(result.moduleStopVerified);
    CHECK(result.writesAttempted == 2U);
    CHECK(result.writesVerified == 1U);
    CHECK(result.rollbackAttempted);
    CHECK(result.rollbackWritesAttempted == 2U);
    CHECK(result.rollbackWritesVerified == 2U);
    CHECK(result.rollbackVerified);
    CHECK_FALSE(result.profileValuesRetained);
    CHECK(result.selectorParkedAtQuadZero);
    CHECK(result.fifoResetSent);
    CHECK(result.readoutResetSent);
    REQUIRE(result.values.size() == 2U);
    CHECK(result.values[1].rollbackVerified);
    CHECK(result.values[0].rollbackVerified);

    const auto operations = DecodeWireOperations(transport);
    REQUIRE(operations.size() == 164U);
    CHECK(operations[157U].address == Base + 0x614AU);
    CHECK(operations[157U].value == 3U);
    CHECK(operations[159U].address == Base + 0x611AU);
    CHECK(operations[159U].value == 250U);
}

TEST_CASE("bulk apply rejects a changed preflight before parameter writes")
{
    using namespace fidget;
    using namespace fidget::test;

    const auto request = MakeRequest();
    FakeCommandTransport transport;
    Open(transport);
    QueueCapture(transport, request.expectedLiveConfiguration, true);

    const std::atomic<bool> cancelled{false};
    const auto result = ApplyFw2051ScpProfile(
        transport, request, cancelled, AllowAllGates());

    CHECK(result.state == ScpBulkApplyState::Failed);
    CHECK_FALSE(result.fullPreflightMatched);
    CHECK_FALSE(result.moduleStopSent);
    CHECK(result.writesAttempted == 0U);
    CHECK(result.message.find("No profile write was sent") !=
          std::string::npos);
    CHECK(DecodeWireOperations(transport).size() == 149U);
}

TEST_CASE("bulk apply passively detaches without rollback after takeover")
{
    using namespace fidget;
    using namespace fidget::test;

    constexpr std::uint32_t Base = 0x11000000U;
    const auto request = MakeRequest();
    FakeCommandTransport transport;
    Open(transport);
    QueueCapture(transport, request.expectedLiveConfiguration);

    TransactionReferences references{0x3C00U, 0x9D200001U};
    QueueWrite(
        transport, references, Base + Fw2051AcquisitionControlRegister, 0U);
    QueueRead(
        transport, references, Base + Fw2051AcquisitionControlRegister, 0U);
    QueueWrite(
        transport, references, Base + Fw2051ScpSelectorRegister, 7U);
    QueueWrite(transport, references, Base + 0x611AU, 200U);
    QueueRead(transport, references, Base + 0x611AU, 200U);

    const std::atomic<bool> cancelled{false};
    const auto gate = [](const std::string& operationName) {
        if (operationName ==
            "the complete SCP profile Sample configuration write")
        {
            return ScpCaptureGateResult{
                ScpCaptureGateStatus::OwnershipLost,
                "A foreign DAQ took ownership."};
        }
        return ScpCaptureGateResult{ScpCaptureGateStatus::Allowed, {}};
    };
    const auto result = ApplyFw2051ScpProfile(
        transport, request, cancelled, gate);

    CHECK(result.state == ScpBulkApplyState::Failed);
    CHECK(result.writesAttempted == 1U);
    CHECK(result.writesVerified == 1U);
    CHECK_FALSE(result.rollbackAttempted);
    CHECK_FALSE(result.selectorParkedAtQuadZero);
    CHECK_FALSE(result.fifoResetSent);
    CHECK_FALSE(result.readoutResetSent);
    CHECK(result.message == "A foreign DAQ took ownership.");
    CHECK(DecodeWireOperations(transport).size() == 154U);
}

TEST_CASE("bulk apply validates the complete target before wire traffic")
{
    using namespace fidget;
    using namespace fidget::test;

    auto request = MakeRequest();
    request.steps = {
        {7, 0x6110U, "Timing filter", 17U, 100U, false},
        {7, 0x6124U, "Shaping time", 167U, 80U, false},
    };
    FakeCommandTransport transport;
    Open(transport);

    const std::atomic<bool> cancelled{false};
    const auto result = ApplyFw2051ScpProfile(
        transport, request, cancelled, AllowAllGates());

    CHECK(result.state == ScpBulkApplyState::Failed);
    CHECK(result.message.find("greater than its shaping") !=
          std::string::npos);
    CHECK(transport.SentRequests().empty());
}
