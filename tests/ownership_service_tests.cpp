#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/VmeProtocol.h"
#include "fake_command_transport.h"
#include "hardware/OwnershipService.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

std::vector<std::byte> EncodeWords(
    const std::vector<std::uint32_t>& words)
{
    return fidget::EncodeMvlcWordsLittleEndian(words.data(), words.size());
}

std::vector<std::byte> MakeReadRequest(
    std::uint16_t address,
    std::uint16_t reference)
{
    const auto words =
        fidget::BuildMvlcLocalRegisterReadRequest(reference, address);
    return fidget::EncodeMvlcWordsLittleEndian(words.data(), words.size());
}

std::vector<std::byte> MakeReadReply(
    std::uint16_t address,
    std::uint16_t reference,
    std::uint32_t value)
{
    return EncodeWords({
        4U,
        0U,
        (static_cast<std::uint32_t>(fidget::MvlcSuperFrameType) << 24U)
            | 3U,
        fidget::MvlcReferenceWordCommand | reference,
        fidget::MvlcReadLocalCommand | address,
        value,
    });
}

void QueueRead(
    fidget::test::FakeCommandTransport& transport,
    std::uint16_t address,
    std::uint16_t reference,
    std::uint32_t value)
{
    transport.QueueExchange({
        MakeReadRequest(address, reference),
        {fidget::test::FakeReceiveAction::Datagram(
            MakeReadReply(address, reference, value))},
    });
}

void QueueIdleProbe(fidget::test::FakeCommandTransport& transport)
{
    QueueRead(transport, fidget::FirmwareRevisionRegister, 1U, 0x0046U);
    QueueRead(transport, fidget::DaqModeRegister, 2U, 0U);
    QueueRead(
        transport,
        fidget::HardwareIdRegister,
        3U,
        fidget::ExpectedMvlcHardwareId);
}

fidget::CrateProject MakeProject()
{
    fidget::CrateProject project;
    project.mvlcHost = "mvlc-test";
    project.mvlcCommandPort = 32768U;
    project.streamHost = "stream-test";
    project.streamPort = 42333U;
    project.modules.push_back({
        "MDPP-32 SCP",
        0x11000000U,
        fidget::MdppBackend::Scp,
        "mdpp1_scp_profile.mwwscp",
    });
    return project;
}

bool WaitFor(
    fidget::OwnershipService& service,
    const std::function<bool(const fidget::TunerSnapshot&)>& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate(*service.CurrentSnapshot()))
        {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return false;
}

void UseProject(fidget::OwnershipService& service)
{
    fidget::UseCrateProjectCommand command;
    command.projectPath = "crate.mwwcrate";
    command.project = MakeProject();
    service.Submit(std::move(command));
    REQUIRE(WaitFor(service, [](const fidget::TunerSnapshot& snapshot) {
        return snapshot.projectActive;
    }));
}

void CheckIdle(
    fidget::OwnershipService& service,
    fidget::test::FakeCommandTransport& transport)
{
    QueueIdleProbe(transport);
    service.Submit(fidget::CheckStatusCommand{});
    REQUIRE(WaitFor(service, [](const fidget::TunerSnapshot& snapshot) {
        return snapshot.ownership
            == fidget::GuidedTunerOwnershipState::Idle;
    }));
}

void ConfirmHandoffAndOpen(
    fidget::OwnershipService& service,
    fidget::test::FakeCommandTransport& transport)
{
    service.Submit(fidget::SetMvmeHandoffConfirmedCommand{true});
    REQUIRE(WaitFor(service, [](const fidget::TunerSnapshot& snapshot) {
        return snapshot.mvmeHandoffConfirmed;
    }));

    QueueIdleProbe(transport);
    service.Submit(fidget::OpenSessionCommand{});
    REQUIRE(WaitFor(service, [](const fidget::TunerSnapshot& snapshot) {
        return snapshot.ownership
            == fidget::GuidedTunerOwnershipState::SessionOpen;
    }));
}

void CheckOnlyReadRequests(
    const fidget::test::FakeCommandTransport& transport)
{
    const auto requests = transport.SentRequests();
    REQUIRE_FALSE(requests.empty());
    for (const auto& request : requests)
    {
        REQUIRE(request.size() == 4U * sizeof(std::uint32_t));
        const auto command = fidget::LoadLittleEndian32(
            request.data() + 2U * sizeof(std::uint32_t));
        CHECK((command & 0xFFFF0000U) == fidget::MvlcReadLocalCommand);
        CHECK((command & 0xFFFF0000U) != fidget::MvlcWriteLocalCommand);
    }
}

} // namespace

TEST_CASE("an idle check permits a confirmed session and release")
{
    using namespace fidget;
    using namespace fidget::test;

    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(std::move(ownedTransport), std::chrono::hours(1));
    UseProject(service);
    CheckIdle(service, *transport);

    const auto idle = service.CurrentSnapshot();
    CHECK(idle->controllerReadingsValid);
    CHECK(idle->mvlcFirmwareRevision == 0x0046U);
    CHECK(idle->mvlcHardwareId == ExpectedMvlcHardwareId);
    CHECK(idle->mvlcDaqMode == 0U);
    CHECK_FALSE(transport->IsOpen());
    CHECK(transport->OpenedHost() == "mvlc-test");
    CHECK(transport->OpenedPort() == 32768U);

    const auto beforeUnconfirmedOpen = idle->revision;
    service.Submit(OpenSessionCommand{});
    REQUIRE(WaitFor(service, [beforeUnconfirmedOpen](const TunerSnapshot& value) {
        return value.revision > beforeUnconfirmedOpen;
    }));
    CHECK(service.CurrentSnapshot()->ownership
          == GuidedTunerOwnershipState::Idle);
    CHECK_FALSE(transport->IsOpen());

    ConfirmHandoffAndOpen(service, *transport);
    CHECK(transport->IsOpen());

    service.Submit(ReleaseSessionCommand{});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.ownership
            == GuidedTunerOwnershipState::Disconnected;
    }));
    CHECK_FALSE(transport->IsOpen());
    CHECK_FALSE(service.CurrentSnapshot()->mvmeHandoffConfirmed);
    CheckOnlyReadRequests(*transport);
}

TEST_CASE("an active DAQ refuses ownership without reading the hardware ID")
{
    using namespace fidget;
    using namespace fidget::test;

    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(std::move(ownedTransport), std::chrono::hours(1));
    UseProject(service);

    QueueRead(*transport, FirmwareRevisionRegister, 1U, 0x0046U);
    QueueRead(*transport, DaqModeRegister, 2U, 0x000FU);
    service.Submit(CheckStatusCommand{});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.ownership == GuidedTunerOwnershipState::InUse;
    }));

    CHECK(service.CurrentSnapshot()->mvlcDaqMode == 0x000FU);
    CHECK(transport->SentRequests().size() == 2U);
    CHECK_FALSE(transport->IsOpen());
    CheckOnlyReadRequests(*transport);
}

TEST_CASE("a stale local-read reply is skipped before the matching reply")
{
    using namespace fidget;
    using namespace fidget::test;

    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(std::move(ownedTransport), std::chrono::hours(1));
    UseProject(service);

    transport->QueueExchange({
        MakeReadRequest(FirmwareRevisionRegister, 1U),
        {
            FakeReceiveAction::Datagram(MakeReadReply(
                FirmwareRevisionRegister, 0x7777U, 0xDEADBEEFU)),
            FakeReceiveAction::Datagram(MakeReadReply(
                FirmwareRevisionRegister, 1U, 0x0046U)),
        },
    });
    QueueRead(*transport, DaqModeRegister, 2U, 0U);
    QueueRead(
        *transport,
        HardwareIdRegister,
        3U,
        ExpectedMvlcHardwareId);

    service.Submit(CheckStatusCommand{});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.ownership == GuidedTunerOwnershipState::Idle;
    }));

    CHECK(service.CurrentSnapshot()->mvlcFirmwareRevision == 0x0046U);
    CHECK(transport->SentRequests().size() == 3U);
    CHECK(transport->ReceiveCapacities().size() == 4U);
    CheckOnlyReadRequests(*transport);
}

TEST_CASE("the idle watchdog passively releases a foreign takeover")
{
    using namespace fidget;
    using namespace fidget::test;

    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(std::move(ownedTransport), 10ms);
    UseProject(service);
    CheckIdle(service, *transport);
    ConfirmHandoffAndOpen(service, *transport);

    QueueRead(*transport, DaqModeRegister, 0x7000U, 0x000FU);
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.ownership
            == GuidedTunerOwnershipState::OwnershipLost;
    }));

    CHECK_FALSE(transport->IsOpen());
    CHECK(transport->SentRequests().size() == 7U);
    CheckOnlyReadRequests(*transport);
}

TEST_CASE("the watchdog reports uncertainty and later recovery")
{
    using namespace fidget;
    using namespace fidget::test;

    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(std::move(ownedTransport), 100ms);
    UseProject(service);
    CheckIdle(service, *transport);

    service.Submit(SetMvmeHandoffConfirmedCommand{true});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.mvmeHandoffConfirmed;
    }));

    QueueIdleProbe(*transport);
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        transport->QueueExchange({
            MakeReadRequest(DaqModeRegister, 0x7000U),
            {FakeReceiveAction::Timeout()},
        });
    }
    service.Submit(OpenSessionCommand{});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return !snapshot.statusMessages.empty()
            && snapshot.statusMessages.back().summary
                == "MVLC command communication is temporarily uncertain. "
                   "No hardware operation is allowed until a later "
                   "watchdog read succeeds: No MVLC response after three "
                   "read-only attempts";
    }));

    CHECK(service.CurrentSnapshot()->ownership
          == GuidedTunerOwnershipState::SessionOpen);
    CHECK(transport->IsOpen());

    QueueRead(*transport, DaqModeRegister, 0x7001U, 0U);
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return !snapshot.statusMessages.empty()
            && snapshot.statusMessages.back().summary
                == "MVLC command communication recovered; DAQ mode is "
                   "still idle and controlled operations are available "
                   "again.";
    }));

    CHECK(service.CurrentSnapshot()->ownership
          == GuidedTunerOwnershipState::SessionOpen);
    CHECK(transport->SentRequests().size() == 10U);
    CheckOnlyReadRequests(*transport);

    service.Submit(ReleaseSessionCommand{});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.ownership
            == GuidedTunerOwnershipState::Disconnected;
    }));
}

TEST_CASE("the pre-write gate blocks when DAQ mode changed")
{
    using namespace fidget;
    using namespace fidget::test;

    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(std::move(ownedTransport), std::chrono::hours(1));
    UseProject(service);
    CheckIdle(service, *transport);
    ConfirmHandoffAndOpen(service, *transport);

    QueueRead(*transport, DaqModeRegister, 4U, 0x000FU);
    const auto gate = service.VerifyPreWriteGate("applying a parameter").get();
    CHECK_FALSE(gate.allowed);
    CHECK(gate.message
          == "A DAQ became active before applying a parameter. The tuner "
             "released its command socket without touching the MDPP, DAQ "
             "mode, or readout stacks.");
    CHECK(service.CurrentSnapshot()->ownership
          == GuidedTunerOwnershipState::OwnershipLost);
    CHECK_FALSE(transport->IsOpen());
    CHECK(transport->SentRequests().size() == 7U);
    CheckOnlyReadRequests(*transport);
}
