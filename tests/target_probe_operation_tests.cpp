#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/VmeProtocol.h"
#include "fake_transport_factory.h"
#include "hardware/TargetProbeOperation.h"
#include "hardware/VmeTransaction.h"
#include "presentation/GuiPresentation.h"
#include "vme_test_support.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

constexpr std::uint32_t TargetBase = 0x11000000U;
constexpr std::uint16_t FirstTargetSuperReference = 2U;
constexpr std::uint32_t FirstTargetStackReference = 1U;
constexpr const char ApprovedActiveUseMessage[] =
    "Active controller use was detected. FIDGET did not take control, and "
    "no hardware settings were changed.";

struct ProbeValues
{
    std::uint32_t mvlcHardwareId =
        fidget::TargetProbeExpectedMvlcHardwareId;
    std::uint32_t mvlcFirmware =
        fidget::TargetProbeExpectedMvlcFirmware;
    std::uint32_t mvlcDaqMode = 0U;
    std::uint16_t targetHardwareId = fidget::Mdpp32HardwareId;
    std::uint16_t targetFirmware =
        fidget::Mdpp32ScpFirmwareRevisionFw2051;
    std::uint16_t targetAcquisition = 0U;
};

fidget::TargetModuleAddress TargetAddress()
{
    const auto parsed = fidget::ParseTargetModuleAddress("0x1100");
    return parsed.address.value();
}

void QueueMvlcRead(
    fidget::test::FakeCommandTransport& transport,
    const ProbeValues& values)
{
    using namespace fidget;
    using namespace fidget::test;

    constexpr std::uint16_t Reference = 1U;
    const auto request = BuildMvlcLocalRegisterBatchReadRequest(
        Reference,
        TargetProbeMvlcRegisterOrder.data(),
        TargetProbeMvlcRegisterOrder.size());
    REQUIRE(request.success);
    const std::array<std::uint32_t, 3U> replyValues{{
        values.mvlcHardwareId,
        values.mvlcFirmware,
        values.mvlcDaqMode,
    }};
    std::vector<std::uint32_t> frame{
        (static_cast<std::uint32_t>(MvlcSuperFrameType) << 24U)
            | static_cast<std::uint32_t>(
                1U + TargetProbeMvlcRegisterOrder.size() * 2U),
        MvlcReferenceWordCommand | Reference,
    };
    for (std::size_t index = 0U;
         index < TargetProbeMvlcRegisterOrder.size();
         ++index)
    {
        frame.push_back(
            MvlcReadLocalCommand | TargetProbeMvlcRegisterOrder[index]);
        frame.push_back(replyValues[index]);
    }
    transport.QueueExchange({
        EncodeWords(request.words),
        {FakeReceiveAction::Datagram(MakeCommandPacket({frame}))},
    });
}

void QueueTargetReads(
    fidget::test::FakeCommandTransport& transport,
    const ProbeValues& values,
    const std::size_t count = 3U)
{
    using namespace fidget;
    using namespace fidget::test;

    const std::array<std::uint16_t, 3U> replyValues{{
        values.targetHardwareId,
        values.targetFirmware,
        values.targetAcquisition,
    }};
    TransactionReferences references{
        FirstTargetSuperReference,
        FirstTargetStackReference,
    };
    for (std::size_t index = 0U; index < count; ++index)
    {
        QueueRead(
            transport,
            references,
            TargetBase + TargetProbeMdppRegisterOrder[index],
            replyValues[index]);
    }
}

struct ProbeFixture
{
    explicit ProbeFixture(const ProbeValues& values, std::size_t targetReads)
        : transportOwner(
            std::make_unique<fidget::test::FakeCommandTransport>())
        , transport(transportOwner.get())
        , factory(std::move(transportOwner))
    {
        QueueMvlcRead(*transport, values);
        QueueTargetReads(*transport, values, targetReads);
    }

    std::unique_ptr<fidget::test::FakeCommandTransport> transportOwner;
    fidget::test::FakeCommandTransport* transport = nullptr;
    fidget::test::FakeTransportFactory factory;
    std::atomic<bool> cancelled{false};
};

fidget::TargetProbeResult Run(
    ProbeFixture& fixture,
    fidget::TransportEndpointRequest endpoint =
        fidget::DirectEthernetEndpointRequest{"mvlc-test", 32768U})
{
    return fidget::RunTargetProbe(
        fixture.factory,
        fidget::TargetProbeRequest{std::move(endpoint), TargetAddress()},
        fixture.cancelled);
}

bool IsImmediateStackPlumbingAddress(const std::uint16_t address)
{
    using namespace fidget;

    const bool stackMemory = address >= MvlcStackMemoryBegin
        && address < MvlcStackMemoryBegin + 0x0100U;
    return stackMemory
        || address == MvlcStackExecutionStatus0
        || address == MvlcStackExecutionStatus1
        || address == MvlcStack0OffsetRegister
        || address == MvlcStack0TriggerRegister;
}

void CheckExactReadOnlyTrace(
    const fidget::test::FakeCommandTransport& transport)
{
    using namespace fidget;
    using namespace fidget::test;

    const auto requests = transport.SentRequests();
    REQUIRE(requests.size() == 7U);

    const auto localWords = DecodeWords(requests.front());
    const auto localRequest = BuildMvlcLocalRegisterBatchReadRequest(
        1U,
        TargetProbeMvlcRegisterOrder.data(),
        TargetProbeMvlcRegisterOrder.size());
    REQUIRE(localRequest.success);
    CHECK(localWords == localRequest.words);

    const std::vector<WireOperation> expectedReads{
        {false, TargetBase + TargetProbeMdppRegisterOrder[0U], 0U},
        {false, TargetBase + TargetProbeMdppRegisterOrder[1U], 0U},
        {false, TargetBase + TargetProbeMdppRegisterOrder[2U], 0U},
    };
    const auto operations = DecodeWireOperations(transport);
    REQUIRE(operations.size() == expectedReads.size());
    for (std::size_t index = 0U; index < operations.size(); ++index)
    {
        CHECK_FALSE(operations[index].write);
        CHECK(operations[index].address == expectedReads[index].address);
        const auto offset = operations[index].address - TargetBase;
        CHECK(offset != Fw2051ScpSelectorRegister);
        CHECK((offset & 0xFF00U) != 0x6100U);
    }

    std::uint16_t superReference = FirstTargetSuperReference;
    std::uint32_t stackReference = FirstTargetStackReference;
    for (std::size_t index = 0U;
         index < TargetProbeMdppRegisterOrder.size();
         ++index)
    {
        const auto operation = EncodeMvlcVmeReadD16Words(
            TargetBase + TargetProbeMdppRegisterOrder[index]);
        const auto expectedUpload = BuildMvlcStackUploadRequest(
            superReference++,
            stackReference++,
            operation.data(),
            operation.size());
        CHECK(DecodeWords(requests[index * 2U + 1U]) == expectedUpload);
        CHECK(DecodeWords(requests[index * 2U + 2U])
              == BuildMvlcStackExecuteRequest(superReference++));
    }

    std::size_t localWriteCount = 0U;
    for (const auto& request : requests)
    {
        const auto words = DecodeWords(request);
        CHECK(std::find(
                  words.begin(), words.end(), MvlcVmeWriteA32D16Command)
              == words.end());
        for (std::size_t index = 0U; index < words.size(); ++index)
        {
            if ((words[index] & 0xFFFF0000U) != MvlcWriteLocalCommand)
                continue;

            ++localWriteCount;
            const auto address = static_cast<std::uint16_t>(words[index]);
            CHECK(IsImmediateStackPlumbingAddress(address));
            CHECK(address != 0x1300U);
            CHECK(address != Fw2051ScpSelectorRegister);
            CHECK((address & 0xFF00U) != 0x6100U);
        }
    }
    CHECK(localWriteCount == 30U);
}

fidget::TunerSnapshot PresentationSnapshot(
    const fidget::TargetProbeResult& result)
{
    using namespace fidget;

    TunerSnapshot snapshot;
    snapshot.target.input.mvlcHost = "mvlc-test";
    snapshot.target.input.moduleAddress = "0x1100";
    snapshot.target.selection = TunerTargetSelection{
        snapshot.target.input,
        TargetAddress(),
    };
    snapshot.target.verification.probedInput = snapshot.target.input;
    snapshot.target.verification.result = result;
    ApplyTargetPresentationEvidence(
        snapshot.target,
        snapshot.tuningSession.evidence);
    snapshot.tuningSession.evidence.endpointInputsValid = true;
    snapshot.tuningSession.evidence.operationIdle = true;
    snapshot.tuningSession.evidence.noRecoveryPending = true;
    return snapshot;
}

void CheckClosed(const ProbeFixture& fixture)
{
    CHECK_FALSE(fixture.transport->IsOpen());
    REQUIRE(fixture.factory.CreateCount() == 1U);
}

} // namespace

TEST_CASE("the target probe has one exact read-only trace over both endpoints")
{
    using namespace fidget;

    TransportEndpointRequest endpoint;
    SUBCASE("direct Ethernet")
    {
        endpoint = DirectEthernetEndpointRequest{"mvlc-test", 32768U};
    }
    SUBCASE("SSH bridge")
    {
        endpoint = SshBridgeEndpointRequest{
            "mvlc-test",
            32768U,
            "bridge-test",
            "fidget_bridge",
        };
    }

    ProbeFixture fixture(ProbeValues{}, 3U);
    const auto result = Run(fixture, endpoint);

    INFO(result.message);
    CHECK(result.outcome == TargetProbeOutcome::VerifiedIdle);
    CHECK(result.temporaryConnectionOpened);
    CHECK(result.temporaryConnectionClosed);
    CHECK(result.evidence.controllerConnected);
    CHECK(result.evidence.controllerIdentityAndFirmwareVerified);
    CHECK(result.evidence.controllerDaqIdleVerified);
    CHECK(result.evidence.targetIdentityAndFirmwareVerified);
    CHECK(result.evidence.targetAcquisitionStoppedVerified);
    CHECK(result.evidence.noControlTaken);
    CHECK(result.evidence.noVmeOrModuleSettingWritesSent);
    CHECK_FALSE(result.evidence.activeControllerUseDetected);
    REQUIRE(result.targetAcquisitionControl.has_value());
    CHECK(*result.targetAcquisitionControl == 0U);
    CheckExactReadOnlyTrace(*fixture.transport);
    CheckClosed(fixture);

    const auto requests = fixture.factory.Requests();
    REQUIRE(requests.size() == 1U);
    CHECK(requests.front().index() == endpoint.index());
    if (const auto* direct =
            std::get_if<DirectEthernetEndpointRequest>(&endpoint))
    {
        const auto& observed =
            std::get<DirectEthernetEndpointRequest>(requests.front());
        CHECK(observed.mvlcHost == direct->mvlcHost);
        CHECK(observed.mvlcCommandPort == direct->mvlcCommandPort);
    }
    else
    {
        const auto& expected =
            std::get<SshBridgeEndpointRequest>(endpoint);
        const auto& observed =
            std::get<SshBridgeEndpointRequest>(requests.front());
        CHECK(observed.mvlcHost == expected.mvlcHost);
        CHECK(observed.mvlcCommandPort == expected.mvlcCommandPort);
        CHECK(observed.sshDestination == expected.sshDestination);
        CHECK(observed.remoteBridgeCommand
              == expected.remoteBridgeCommand);
    }
}

TEST_CASE("active MVLC DAQ routes to controller conflict without target traffic")
{
    using namespace fidget;

    ProbeValues values;
    values.mvlcDaqMode = 0x0005U;
    ProbeFixture fixture(values, 0U);
    const auto result = Run(fixture);

    CHECK(result.outcome == TargetProbeOutcome::ControllerDaqActive);
    CHECK(result.message == ApprovedActiveUseMessage);
    CHECK(result.evidence.activeControllerUseDetected);
    CHECK(result.evidence.noControlTaken);
    CHECK(result.evidence.noVmeOrModuleSettingWritesSent);
    CHECK(fixture.transport->SentRequests().size() == 1U);
    CHECK(DecodeWireOperations(*fixture.transport).empty());
    const auto view = PresentGui(PresentationSnapshot(result));
    CHECK(view.page == GuiPage::ControllerConflict);
    CHECK(view.claims.noControlTaken);
    CHECK(view.claims.noVmeOrModuleSettingWritesSent);
    CheckClosed(fixture);
}

TEST_CASE("an active target routes to controller conflict after read-only checks")
{
    using namespace fidget;

    ProbeValues values;
    values.targetAcquisition = 1U;
    ProbeFixture fixture(values, 3U);
    const auto result = Run(fixture);

    CHECK(result.outcome == TargetProbeOutcome::TargetAcquisitionActive);
    CHECK(result.message == ApprovedActiveUseMessage);
    CHECK(result.evidence.activeControllerUseDetected);
    CHECK(result.evidence.targetIdentityAndFirmwareVerified);
    CHECK_FALSE(result.evidence.targetAcquisitionStoppedVerified);
    CheckExactReadOnlyTrace(*fixture.transport);
    const auto view = PresentGui(PresentationSnapshot(result));
    CHECK(view.page == GuiPage::ControllerConflict);
    CHECK(view.claims.noControlTaken);
    CHECK(view.claims.noVmeOrModuleSettingWritesSent);
    CheckClosed(fixture);
}

TEST_CASE("wrong MVLC identity and firmware have specific outcomes")
{
    using namespace fidget;

    ProbeValues values;
    SUBCASE("identity")
        values.mvlcHardwareId = 0x1234U;
    SUBCASE("firmware")
        values.mvlcFirmware = 0x0045U;

    ProbeFixture fixture(values, 0U);
    const auto result = Run(fixture);
    CHECK(result.outcome == (
        values.mvlcHardwareId == TargetProbeExpectedMvlcHardwareId
            ? TargetProbeOutcome::WrongMvlcFirmware
            : TargetProbeOutcome::WrongMvlcIdentity));
    CHECK(result.evidence.controllerConnected);
    CHECK_FALSE(result.evidence.controllerIdentityAndFirmwareVerified);
    CHECK_FALSE(result.evidence.activeControllerUseDetected);
    CHECK(DecodeWireOperations(*fixture.transport).empty());
    CheckClosed(fixture);
}

TEST_CASE("wrong target identity and firmware have specific outcomes")
{
    using namespace fidget;

    ProbeValues values;
    std::size_t targetReads = 1U;
    SUBCASE("identity")
    {
        values.targetHardwareId = 0x1234U;
    }
    SUBCASE("firmware")
    {
        values.targetFirmware = 0x2050U;
        targetReads = 2U;
    }

    ProbeFixture fixture(values, targetReads);
    const auto result = Run(fixture);
    CHECK(result.outcome == (
        values.targetHardwareId == 0x1234U
            ? TargetProbeOutcome::WrongTargetIdentity
            : TargetProbeOutcome::WrongTargetFirmware));
    CHECK(result.evidence.controllerIdentityAndFirmwareVerified);
    CHECK(result.evidence.controllerDaqIdleVerified);
    CHECK_FALSE(result.evidence.targetIdentityAndFirmwareVerified);
    const auto operations = DecodeWireOperations(*fixture.transport);
    REQUIRE(operations.size() == targetReads);
    CHECK(std::none_of(
        operations.begin(), operations.end(),
        [](const auto& operation) { return operation.write; }));
    CheckClosed(fixture);
}

TEST_CASE("both MDPP-32 identities require exact SCP FW2051")
{
    using namespace fidget;

    for (const auto hardwareId :
         {Mdpp32HardwareId, Mdpp32AlternateHardwareId})
    {
        CAPTURE(hardwareId);

        ProbeValues accepted;
        accepted.targetHardwareId = hardwareId;
        ProbeFixture acceptedFixture(accepted, 3U);
        const auto acceptedResult = Run(acceptedFixture);
        CHECK(acceptedResult.outcome == TargetProbeOutcome::VerifiedIdle);
        CHECK(acceptedResult.evidence.targetIdentityAndFirmwareVerified);
        CheckExactReadOnlyTrace(*acceptedFixture.transport);
        CheckClosed(acceptedFixture);

        ProbeValues wrongFirmware;
        wrongFirmware.targetHardwareId = hardwareId;
        wrongFirmware.targetFirmware = 0x2050U;
        ProbeFixture wrongFirmwareFixture(wrongFirmware, 2U);
        const auto wrongFirmwareResult = Run(wrongFirmwareFixture);
        CHECK(wrongFirmwareResult.outcome
              == TargetProbeOutcome::WrongTargetFirmware);
        CHECK_FALSE(
            wrongFirmwareResult.evidence.targetIdentityAndFirmwareVerified);
        const auto operations =
            DecodeWireOperations(*wrongFirmwareFixture.transport);
        REQUIRE(operations.size() == 2U);
        CHECK(std::none_of(
            operations.begin(), operations.end(),
            [](const auto& operation) { return operation.write; }));
        CheckClosed(wrongFirmwareFixture);
    }
}

TEST_CASE("target probe timeout is specific and closes the connection")
{
    using namespace fidget;
    using namespace fidget::test;

    auto owner = std::make_unique<FakeCommandTransport>();
    auto* transport = owner.get();
    const auto request = BuildMvlcLocalRegisterBatchReadRequest(
        1U,
        TargetProbeMvlcRegisterOrder.data(),
        TargetProbeMvlcRegisterOrder.size());
    REQUIRE(request.success);
    for (int attempt = 0; attempt < MvlcFingerprintReadAttemptCount; ++attempt)
    {
        transport->QueueExchange({
            EncodeWords(request.words),
            {FakeReceiveAction::Timeout()},
        });
    }
    FakeTransportFactory factory(std::move(owner));
    const std::atomic<bool> cancelled{false};
    const auto result = RunTargetProbe(
        factory,
        TargetProbeRequest{
            DirectEthernetEndpointRequest{"mvlc-test", 32768U},
            TargetAddress(),
        },
        cancelled);

    CHECK(result.outcome == TargetProbeOutcome::Timeout);
    CHECK(result.temporaryConnectionClosed);
    CHECK(result.evidence.noVmeOrModuleSettingWritesSent);
    CHECK(transport->SentRequests().size()
          == static_cast<std::size_t>(MvlcFingerprintReadAttemptCount));
    CHECK(DecodeWireOperations(*transport).empty());
    CHECK_FALSE(transport->IsOpen());
}

TEST_CASE("malformed local and VME responses have a specific outcome")
{
    using namespace fidget;
    using namespace fidget::test;

    auto owner = std::make_unique<FakeCommandTransport>();
    auto* transport = owner.get();

    SUBCASE("local response")
    {
        const auto request = BuildMvlcLocalRegisterBatchReadRequest(
            1U,
            TargetProbeMvlcRegisterOrder.data(),
            TargetProbeMvlcRegisterOrder.size());
        REQUIRE(request.success);
        transport->QueueExchange({
            EncodeWords(request.words),
            {FakeReceiveAction::Datagram(
                std::vector<std::byte>{std::byte{0x01U}})},
        });
    }
    SUBCASE("VME response")
    {
        QueueMvlcRead(*transport, ProbeValues{});
        const auto operation = EncodeMvlcVmeReadD16Words(
            TargetBase + TargetProbeMdppRegisterOrder[0U]);
        const auto upload = BuildMvlcStackUploadRequest(
            FirstTargetSuperReference,
            FirstTargetStackReference,
            operation.data(),
            operation.size());
        transport->QueueExchange({
            EncodeWords(upload),
            {FakeReceiveAction::Datagram(MakeCommandPacket({
                MakeSuperFrame(FirstTargetSuperReference),
            }))},
        });
        transport->QueueExchange({
            EncodeWords(BuildMvlcStackExecuteRequest(
                FirstTargetSuperReference + 1U)),
            {FakeReceiveAction::Datagram(MakeCommandPacket({
                MakeSuperFrame(FirstTargetSuperReference + 1U),
                {
                    (static_cast<std::uint32_t>(MvlcStackFrameType) << 24U)
                        | 3U,
                    FirstTargetStackReference,
                    Mdpp32HardwareId,
                    0xBAD0U,
                },
            }))},
        });
    }

    FakeTransportFactory factory(std::move(owner));
    const std::atomic<bool> cancelled{false};
    const auto result = RunTargetProbe(
        factory,
        TargetProbeRequest{
            DirectEthernetEndpointRequest{"mvlc-test", 32768U},
            TargetAddress(),
        },
        cancelled);

    INFO(result.message);
    CHECK(result.outcome == TargetProbeOutcome::MalformedResponse);
    CHECK(result.temporaryConnectionClosed);
    CHECK(result.evidence.noVmeOrModuleSettingWritesSent);
    CHECK_FALSE(transport->IsOpen());
}
