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
constexpr std::uint16_t FirstTargetSuperReference = 1U;
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

struct ProbeFixture
{
    ProbeFixture()
        : transportOwner(
            std::make_unique<fidget::test::FakeCommandTransport>())
        , transport(transportOwner.get())
        , factory(std::move(transportOwner))
    {
    }

    std::unique_ptr<fidget::test::FakeCommandTransport> transportOwner;
    fidget::test::FakeCommandTransport* transport = nullptr;
    fidget::test::FakeTransportFactory factory;
    std::atomic<bool> cancelled{false};
};

fidget::TargetModuleAddress TargetAddress()
{
    const auto parsed = fidget::ParseTargetModuleAddress("0x1100");
    return parsed.address.value();
}

fidget::TransportEndpointRequest DirectEndpoint()
{
    return fidget::DirectEthernetEndpointRequest{"mvlc-test", 32768U};
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

fidget::ControllerProbeResult RunController(
    ProbeFixture& fixture,
    fidget::TransportEndpointRequest endpoint = DirectEndpoint())
{
    return fidget::RunControllerProbe(
        fixture.factory,
        fidget::ControllerProbeRequest{std::move(endpoint)},
        fixture.cancelled);
}

fidget::TargetProbeResult RunTarget(
    ProbeFixture& fixture,
    fidget::TransportEndpointRequest endpoint = DirectEndpoint())
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

void CheckExactControllerTrace(
    const fidget::test::FakeCommandTransport& transport)
{
    using namespace fidget;
    using namespace fidget::test;

    const auto requests = transport.SentRequests();
    REQUIRE(requests.size() == 1U);
    const auto expected = BuildMvlcLocalRegisterBatchReadRequest(
        1U,
        TargetProbeMvlcRegisterOrder.data(),
        TargetProbeMvlcRegisterOrder.size());
    REQUIRE(expected.success);
    const auto words = DecodeWords(requests.front());
    CHECK(words == expected.words);
    CHECK(DecodeWireOperations(transport).empty());
    CHECK(std::none_of(
        words.begin(), words.end(), [](const std::uint32_t word) {
            return (word & 0xFFFF0000U) == MvlcWriteLocalCommand
                || word == MvlcVmeWriteA32D16Command;
        }));
}

void CheckExactTargetTrace(
    const fidget::test::FakeCommandTransport& transport,
    const std::size_t readCount = 3U)
{
    using namespace fidget;
    using namespace fidget::test;

    const auto requests = transport.SentRequests();
    REQUIRE(requests.size() == readCount * 2U);
    const auto operations = DecodeWireOperations(transport);
    REQUIRE(operations.size() == readCount);

    std::uint16_t superReference = FirstTargetSuperReference;
    std::uint32_t stackReference = FirstTargetStackReference;
    for (std::size_t index = 0U; index < readCount; ++index)
    {
        CHECK_FALSE(operations[index].write);
        CHECK(operations[index].address
              == TargetBase + TargetProbeMdppRegisterOrder[index]);
        const auto offset = operations[index].address - TargetBase;
        CHECK(offset != Fw2051ScpSelectorRegister);
        CHECK((offset & 0xFF00U) != 0x6100U);

        const auto operation = EncodeMvlcVmeReadD16Words(
            TargetBase + TargetProbeMdppRegisterOrder[index]);
        const auto expectedUpload = BuildMvlcStackUploadRequest(
            superReference++,
            stackReference++,
            operation.data(),
            operation.size());
        CHECK(DecodeWords(requests[index * 2U]) == expectedUpload);
        CHECK(DecodeWords(requests[index * 2U + 1U])
              == BuildMvlcStackExecuteRequest(superReference++));
    }

    std::size_t localWriteCount = 0U;
    for (const auto& request : requests)
    {
        const auto words = DecodeWords(request);
        CHECK(std::find(
                  words.begin(), words.end(), MvlcVmeWriteA32D16Command)
              == words.end());
        for (const auto word : words)
        {
            if ((word & 0xFFFF0000U) != MvlcWriteLocalCommand)
                continue;
            ++localWriteCount;
            const auto address = static_cast<std::uint16_t>(word);
            CHECK(IsImmediateStackPlumbingAddress(address));
            CHECK(address != 0x1300U);
            CHECK(address != Fw2051ScpSelectorRegister);
            CHECK((address & 0xFF00U) != 0x6100U);
        }
    }
    CHECK(localWriteCount == readCount * 10U);
}

fidget::ControllerProbeResult VerifiedControllerResult()
{
    using namespace fidget;
    ControllerProbeResult result;
    result.outcome = ControllerProbeOutcome::VerifiedIdle;
    result.evidence.controllerConnected = true;
    result.evidence.controllerIdentityAndFirmwareVerified = true;
    result.evidence.controllerDaqIdleVerified = true;
    result.evidence.noControlTaken = true;
    result.evidence.noVmeOrModuleSettingWritesSent = true;
    return result;
}

fidget::TunerSnapshot PresentationSnapshot(
    const fidget::ControllerProbeResult& controller,
    const fidget::TargetProbeResult& target = {})
{
    using namespace fidget;

    TunerSnapshot snapshot;
    snapshot.target.input.mvlcHost = "mvlc-test";
    snapshot.target.input.moduleAddress = "0x1100";
    snapshot.target.selection = TunerTargetSelection{
        snapshot.target.input,
        TargetAddress(),
    };
    snapshot.target.controllerVerification.probedEndpoint =
        ControllerEndpointForTarget(snapshot.target.input);
    snapshot.target.controllerVerification.result = controller;
    snapshot.target.verification.probedInput = snapshot.target.input;
    snapshot.target.verification.result = target;
    ApplyTargetPresentationEvidence(
        snapshot.target,
        snapshot.tuningSession.evidence);
    snapshot.tuningSession.evidence.endpointInputsValid = true;
    snapshot.tuningSession.evidence.targetModuleAddressValid = true;
    snapshot.tuningSession.evidence.operationIdle = true;
    snapshot.tuningSession.evidence.noRecoveryPending = true;
    return snapshot;
}

void CheckClosed(const ProbeFixture& fixture)
{
    CHECK_FALSE(fixture.transport->IsOpen());
    REQUIRE(fixture.factory.CreateCount() == 1U);
}

void CheckEndpointRequest(
    const fidget::test::FakeTransportFactory& factory,
    const fidget::TransportEndpointRequest& expected)
{
    using namespace fidget;

    const auto requests = factory.Requests();
    REQUIRE(requests.size() == 1U);
    CHECK(requests.front().index() == expected.index());
    if (const auto* direct =
            std::get_if<DirectEthernetEndpointRequest>(&expected))
    {
        const auto& observed =
            std::get<DirectEthernetEndpointRequest>(requests.front());
        CHECK(observed.mvlcHost == direct->mvlcHost);
        CHECK(observed.mvlcCommandPort == direct->mvlcCommandPort);
        return;
    }

    const auto& bridge = std::get<SshBridgeEndpointRequest>(expected);
    const auto& observed =
        std::get<SshBridgeEndpointRequest>(requests.front());
    CHECK(observed.mvlcHost == bridge.mvlcHost);
    CHECK(observed.mvlcCommandPort == bridge.mvlcCommandPort);
    CHECK(observed.sshDestination == bridge.sshDestination);
    CHECK(observed.remoteBridgeCommand == bridge.remoteBridgeCommand);
}

} // namespace

TEST_CASE("Connect has one controller-only trace over both endpoints")
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
            "mvlc-test", 32768U, "bridge-test", "fidget_bridge"};
    }

    ProbeFixture fixture;
    QueueMvlcRead(*fixture.transport, ProbeValues{});
    const auto result = RunController(fixture, endpoint);

    INFO(result.message);
    CHECK(result.outcome == ControllerProbeOutcome::VerifiedIdle);
    CHECK(result.temporaryConnectionOpened);
    CHECK(result.temporaryConnectionClosed);
    CHECK(result.evidence.controllerConnected);
    CHECK(result.evidence.controllerIdentityAndFirmwareVerified);
    CHECK(result.evidence.controllerDaqIdleVerified);
    CHECK(result.evidence.noControlTaken);
    CHECK(result.evidence.noVmeOrModuleSettingWritesSent);
    CHECK_FALSE(result.evidence.activeControllerUseDetected);
    CheckExactControllerTrace(*fixture.transport);
    CheckClosed(fixture);
    CheckEndpointRequest(fixture.factory, endpoint);
}

TEST_CASE("Check has one target-only trace over both endpoints")
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
            "mvlc-test", 32768U, "bridge-test", "fidget_bridge"};
    }

    ProbeFixture fixture;
    QueueTargetReads(*fixture.transport, ProbeValues{});
    const auto result = RunTarget(fixture, endpoint);

    INFO(result.message);
    CHECK(result.outcome == TargetProbeOutcome::VerifiedIdle);
    CHECK(result.temporaryConnectionOpened);
    CHECK(result.temporaryConnectionClosed);
    CHECK(result.evidence.targetIdentityAndFirmwareVerified);
    CHECK(result.evidence.targetAcquisitionStoppedVerified);
    CHECK(result.evidence.noControlTaken);
    CHECK(result.evidence.noVmeOrModuleSettingWritesSent);
    CHECK_FALSE(result.evidence.activeControllerUseDetected);
    CheckExactTargetTrace(*fixture.transport);
    CheckClosed(fixture);
    CheckEndpointRequest(fixture.factory, endpoint);
}

TEST_CASE("active MVLC DAQ routes to conflict with controller-only traffic")
{
    using namespace fidget;

    ProbeValues values;
    values.mvlcDaqMode = 0x0005U;
    ProbeFixture fixture;
    QueueMvlcRead(*fixture.transport, values);
    const auto result = RunController(fixture);

    CHECK(result.outcome == ControllerProbeOutcome::ControllerDaqActive);
    CHECK(result.message == ApprovedActiveUseMessage);
    CHECK(result.evidence.activeControllerUseDetected);
    CheckExactControllerTrace(*fixture.transport);
    const auto view = PresentGui(PresentationSnapshot(result));
    CHECK(view.page == GuiPage::ControllerConflict);
    CHECK(view.claims.noControlTaken);
    CHECK(view.claims.noVmeOrModuleSettingWritesSent);
    CheckClosed(fixture);
}

TEST_CASE("an active target routes to conflict after target-only reads")
{
    using namespace fidget;

    ProbeValues values;
    values.targetAcquisition = 1U;
    ProbeFixture fixture;
    QueueTargetReads(*fixture.transport, values);
    const auto result = RunTarget(fixture);

    CHECK(result.outcome == TargetProbeOutcome::TargetAcquisitionActive);
    CHECK(result.message == ApprovedActiveUseMessage);
    CHECK(result.evidence.activeControllerUseDetected);
    CHECK(result.evidence.targetIdentityAndFirmwareVerified);
    CHECK_FALSE(result.evidence.targetAcquisitionStoppedVerified);
    CheckExactTargetTrace(*fixture.transport);
    const auto view = PresentGui(
        PresentationSnapshot(VerifiedControllerResult(), result));
    CHECK(view.page == GuiPage::ControllerConflict);
    CHECK(view.claims.noControlTaken);
    CHECK(view.claims.noVmeOrModuleSettingWritesSent);
    CheckClosed(fixture);
}

TEST_CASE("wrong MVLC identity and firmware have controller outcomes")
{
    using namespace fidget;

    ProbeValues values;
    SUBCASE("identity")
        values.mvlcHardwareId = 0x1234U;
    SUBCASE("firmware")
        values.mvlcFirmware = 0x0045U;

    ProbeFixture fixture;
    QueueMvlcRead(*fixture.transport, values);
    const auto result = RunController(fixture);
    CHECK(result.outcome == (
        values.mvlcHardwareId == TargetProbeExpectedMvlcHardwareId
            ? ControllerProbeOutcome::WrongMvlcFirmware
            : ControllerProbeOutcome::WrongMvlcIdentity));
    CHECK(result.evidence.controllerConnected);
    CHECK_FALSE(result.evidence.controllerIdentityAndFirmwareVerified);
    CheckExactControllerTrace(*fixture.transport);
    CheckClosed(fixture);
}

TEST_CASE("wrong target identity and firmware have target outcomes")
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

    ProbeFixture fixture;
    QueueTargetReads(*fixture.transport, values, targetReads);
    const auto result = RunTarget(fixture);
    CHECK(result.outcome == (
        values.targetHardwareId == 0x1234U
            ? TargetProbeOutcome::WrongTargetIdentity
            : TargetProbeOutcome::WrongTargetFirmware));
    CHECK_FALSE(result.evidence.targetIdentityAndFirmwareVerified);
    CheckExactTargetTrace(*fixture.transport, targetReads);
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
        ProbeFixture acceptedFixture;
        QueueTargetReads(*acceptedFixture.transport, accepted);
        const auto acceptedResult = RunTarget(acceptedFixture);
        CHECK(acceptedResult.outcome == TargetProbeOutcome::VerifiedIdle);
        CHECK(acceptedResult.evidence.targetIdentityAndFirmwareVerified);
        CheckExactTargetTrace(*acceptedFixture.transport);
        CheckClosed(acceptedFixture);

        ProbeValues wrongFirmware;
        wrongFirmware.targetHardwareId = hardwareId;
        wrongFirmware.targetFirmware = 0x2050U;
        ProbeFixture wrongFirmwareFixture;
        QueueTargetReads(*wrongFirmwareFixture.transport, wrongFirmware, 2U);
        const auto wrongFirmwareResult = RunTarget(wrongFirmwareFixture);
        CHECK(wrongFirmwareResult.outcome
              == TargetProbeOutcome::WrongTargetFirmware);
        CHECK_FALSE(
            wrongFirmwareResult.evidence.targetIdentityAndFirmwareVerified);
        CheckExactTargetTrace(*wrongFirmwareFixture.transport, 2U);
        CheckClosed(wrongFirmwareFixture);
    }
}

TEST_CASE("controller and target timeouts stay stage-specific")
{
    using namespace fidget;
    using namespace fidget::test;

    SUBCASE("controller")
    {
        ProbeFixture fixture;
        const auto request = BuildMvlcLocalRegisterBatchReadRequest(
            1U,
            TargetProbeMvlcRegisterOrder.data(),
            TargetProbeMvlcRegisterOrder.size());
        REQUIRE(request.success);
        for (int attempt = 0;
             attempt < MvlcFingerprintReadAttemptCount;
             ++attempt)
        {
            fixture.transport->QueueExchange({
                EncodeWords(request.words),
                {FakeReceiveAction::Timeout()},
            });
        }
        const auto result = RunController(fixture);
        CHECK(result.outcome == ControllerProbeOutcome::Timeout);
        CHECK(result.temporaryConnectionClosed);
        CHECK(DecodeWireOperations(*fixture.transport).empty());
        CheckClosed(fixture);
    }

    SUBCASE("target")
    {
        ProbeFixture fixture;
        for (int attempt = 0;
             attempt < MvlcTransactionAttemptCount;
             ++attempt)
        {
            const auto super = static_cast<std::uint16_t>(1U + attempt * 2U);
            const auto stack = static_cast<std::uint32_t>(1U + attempt);
            const auto operation = EncodeMvlcVmeReadD16Words(
                TargetBase + TargetProbeMdppRegisterOrder[0U]);
            const auto upload = BuildMvlcStackUploadRequest(
                super, stack, operation.data(), operation.size());
            fixture.transport->QueueExchange({
                EncodeWords(upload),
                {FakeReceiveAction::Datagram(
                    MakeCommandPacket({MakeSuperFrame(super)}))},
            });
            fixture.transport->QueueExchange({
                EncodeWords(BuildMvlcStackExecuteRequest(super + 1U)),
                {FakeReceiveAction::Timeout()},
            });
        }
        const auto result = RunTarget(fixture);
        CHECK(result.outcome == TargetProbeOutcome::Timeout);
        CHECK(result.temporaryConnectionClosed);
        const auto operations = DecodeWireOperations(*fixture.transport);
        REQUIRE(operations.size()
                == static_cast<std::size_t>(MvlcTransactionAttemptCount));
        CHECK(std::none_of(
            operations.begin(), operations.end(),
            [](const auto& operation) { return operation.write; }));
        CheckClosed(fixture);
    }
}

TEST_CASE("malformed controller and target responses stay stage-specific")
{
    using namespace fidget;
    using namespace fidget::test;

    SUBCASE("controller")
    {
        ProbeFixture fixture;
        const auto request = BuildMvlcLocalRegisterBatchReadRequest(
            1U,
            TargetProbeMvlcRegisterOrder.data(),
            TargetProbeMvlcRegisterOrder.size());
        REQUIRE(request.success);
        fixture.transport->QueueExchange({
            EncodeWords(request.words),
            {FakeReceiveAction::Datagram(
                std::vector<std::byte>{std::byte{0x01U}})},
        });
        const auto result = RunController(fixture);
        CHECK(result.outcome == ControllerProbeOutcome::MalformedResponse);
        CHECK(result.temporaryConnectionClosed);
        CheckClosed(fixture);
    }

    SUBCASE("target")
    {
        ProbeFixture fixture;
        const auto operation = EncodeMvlcVmeReadD16Words(
            TargetBase + TargetProbeMdppRegisterOrder[0U]);
        const auto upload = BuildMvlcStackUploadRequest(
            FirstTargetSuperReference,
            FirstTargetStackReference,
            operation.data(),
            operation.size());
        fixture.transport->QueueExchange({
            EncodeWords(upload),
            {FakeReceiveAction::Datagram(MakeCommandPacket({
                MakeSuperFrame(FirstTargetSuperReference),
            }))},
        });
        fixture.transport->QueueExchange({
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
        const auto result = RunTarget(fixture);
        CHECK(result.outcome == TargetProbeOutcome::MalformedResponse);
        CHECK(result.temporaryConnectionClosed);
        CHECK(result.evidence.noVmeOrModuleSettingWritesSent);
        CheckClosed(fixture);
    }
}
