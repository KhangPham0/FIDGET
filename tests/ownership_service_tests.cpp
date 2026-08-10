#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/VmeProtocol.h"
#include "core/StartupAudit.h"
#include "fake_command_transport.h"
#include "hardware/OwnershipService.h"

#include <algorithm>
#include <chrono>
#include <array>
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
    std::uint16_t value)
{
    return {
        (static_cast<std::uint32_t>(fidget::MvlcStackFrameType) << 24U)
            | 2U,
        stackReference,
        value,
    };
}

std::vector<std::byte> MakeUploadRequest(
    std::uint16_t superReference,
    std::uint32_t stackReference,
    std::uint32_t address)
{
    const auto operation = fidget::EncodeMvlcVmeReadD16Words(address);
    const auto request = fidget::BuildMvlcStackUploadRequest(
        superReference,
        stackReference,
        operation.data(),
        operation.size());
    return EncodeWords(request);
}

std::vector<std::byte> MakeExecuteRequest(std::uint16_t superReference)
{
    return EncodeWords(fidget::BuildMvlcStackExecuteRequest(superReference));
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

using AuditValues = std::array<
    std::uint16_t,
    fidget::Fw2051StartupAuditRegisterCount>;

std::size_t AuditRegisterIndex(std::uint16_t registerOffset)
{
    const auto& table = fidget::Fw2051StartupAuditRegisterTable;
    const auto found = std::find_if(
        table.begin(), table.end(),
        [registerOffset](const auto& definition) {
            return definition.registerOffset == registerOffset;
        });
    REQUIRE(found != table.end());
    return static_cast<std::size_t>(found - table.begin());
}

AuditValues MakeReadyAuditValues()
{
    AuditValues values{};
    const auto set = [&values](
                         const std::uint16_t registerOffset,
                         const std::uint16_t value) {
        values[AuditRegisterIndex(registerOffset)] = value;
    };

    set(0x6004U, 0x0011U);
    set(0x6006U, 1U);
    set(0x6008U, 0x5007U);
    set(0x600EU, 0x2051U);
    set(0x6010U, 1U);
    set(0x6012U, 0U);
    set(0x6018U, 1U);
    set(0x601AU, 1U);
    set(0x601CU, 1U);
    set(0x601EU, 1U);
    set(0x6032U, 2U);
    set(0x6036U, 3U);
    set(0x6038U, 3U);
    set(0x603AU, 0U);
    set(0x6042U, 5U);
    set(0x6044U, 0x18U);
    set(0x6046U, 0U);
    set(0x6050U, 0x3FF0U);
    set(0x6054U, 32U);
    set(0x6058U, 0x0100U);
    set(0x605CU, 1U);
    set(0x605EU, 0x0100U);
    set(0x6060U, 0U);
    set(0x6062U, 0U);
    set(0x6064U, 0U);
    set(0x6066U, 0U);
    set(0x6068U, 1U);
    set(0x606AU, 1U);
    set(0x606CU, 1U);
    set(0x6070U, 0U);
    set(0x6072U, 400U);
    set(0x6074U, 1U);
    set(0x607AU, 0U);
    set(0x607CU, 0U);
    set(0x607EU, 0U);
    set(0x6096U, 0U);
    set(0x6098U, 1U);
    return values;
}

void QueueStartupAudit(
    fidget::test::FakeCommandTransport& transport,
    const AuditValues& values)
{
    QueueRead(transport, fidget::DaqModeRegister, 4U, 0U);

    std::uint16_t superReference = 0x1600U;
    std::uint32_t stackReference = 0x9C080001U;
    for (std::size_t index = 0U;
         index < fidget::Fw2051StartupAuditRegisterTable.size(); ++index)
    {
        const auto address = 0x11000000U +
            fidget::Fw2051StartupAuditRegisterTable[index].registerOffset;
        transport.QueueExchange({
            MakeUploadRequest(superReference, stackReference, address),
            {fidget::test::FakeReceiveAction::Datagram(
                MakeCommandPacket({MakeSuperFrame(superReference)}))},
        });
        ++superReference;
        transport.QueueExchange({
            MakeExecuteRequest(superReference),
            {fidget::test::FakeReceiveAction::Datagram(
                MakeCommandPacket({
                    MakeSuperFrame(superReference),
                    MakeReadStackFrame(stackReference, values[index]),
                }))},
        });
        ++superReference;
        ++stackReference;
    }
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

void CheckStartupAuditWireRequests(
    const fidget::test::FakeCommandTransport& transport)
{
    std::size_t readOperations = 0U;
    std::size_t writeOperations = 0U;
    std::vector<std::uint32_t> readAddresses;
    for (const auto& request : transport.SentRequests())
    {
        const auto words = DecodeWords(request);
        for (std::size_t index = 0U; index < words.size(); ++index)
        {
            if (words[index] == fidget::MvlcVmeReadA32D16Command)
            {
                ++readOperations;
                REQUIRE(index + 2U < words.size());
                readAddresses.push_back(words[index + 2U]);
            }
            if (words[index] == fidget::MvlcVmeWriteA32D16Command)
            {
                ++writeOperations;
            }
        }
    }

    CHECK(readOperations == 37U);
    CHECK(writeOperations == 0U);
    REQUIRE(readAddresses.size() ==
            fidget::Fw2051StartupAuditRegisterTable.size());
    for (std::size_t index = 0U; index < readAddresses.size(); ++index)
    {
        CHECK(readAddresses[index] ==
              0x11000000U +
                  fidget::Fw2051StartupAuditRegisterTable[index]
                      .registerOffset);
    }
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

TEST_CASE("the startup audit reads all 37 registers without a VME write")
{
    using namespace fidget;
    using namespace fidget::test;

    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(std::move(ownedTransport), std::chrono::hours(1));
    UseProject(service);
    CheckIdle(service, *transport);
    ConfirmHandoffAndOpen(service, *transport);

    const auto values = MakeReadyAuditValues();
    QueueStartupAudit(*transport, values);
    service.Submit(RunStartupAuditCommand{});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.startupAudit.state == StartupAuditState::Complete;
    }));

    const auto snapshot = service.CurrentSnapshot();
    CHECK(snapshot->ownership == GuidedTunerOwnershipState::SessionOpen);
    CHECK(snapshot->activeOperation == GuidedTunerOperation::None);
    CHECK(snapshot->startupAuditCompleteForTarget);
    CHECK(snapshot->startupAuditReady);
    CHECK(snapshot->startupAudit.baseAddress == 0x11000000U);
    CHECK(snapshot->startupAudit.hardwareId == 0x5007U);
    CHECK(snapshot->startupAudit.firmwareRevision == 0x2051U);
    CHECK(snapshot->startupAudit.registersRead == 37U);
    CHECK(snapshot->startupAudit.rows.size() == 37U);
    CHECK(snapshot->startupAudit.requiredChecks == 7U);
    CHECK(snapshot->startupAudit.requiredReady == 7U);
    CHECK(snapshot->startupAudit.blockingIssues == 0U);
    CHECK(snapshot->startupAudit.warnings == 0U);
    CHECK_FALSE(snapshot->startupAudit.vmeWritesIssued);
    CHECK(transport->SentRequests().size() == 81U);
    CheckStartupAuditWireRequests(*transport);
}

TEST_CASE("a blocked startup audit publishes the prototype counts")
{
    using namespace fidget;
    using namespace fidget::test;

    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(std::move(ownedTransport), std::chrono::hours(1));
    UseProject(service);
    CheckIdle(service, *transport);
    ConfirmHandoffAndOpen(service, *transport);

    auto values = MakeReadyAuditValues();
    values[AuditRegisterIndex(0x6044U)] = 0x08U;
    values[AuditRegisterIndex(0x603AU)] = 1U;
    values[AuditRegisterIndex(0x6070U)] = 1U;
    QueueStartupAudit(*transport, values);
    service.Submit(RunStartupAuditCommand{});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.startupAudit.state == StartupAuditState::Complete;
    }));

    const auto snapshot = service.CurrentSnapshot();
    CHECK(snapshot->startupAuditCompleteForTarget);
    CHECK_FALSE(snapshot->startupAuditReady);
    CHECK(snapshot->startupAudit.requiredChecks == 7U);
    CHECK(snapshot->startupAudit.requiredReady == 6U);
    CHECK(snapshot->startupAudit.blockingIssues == 1U);
    CHECK(snapshot->startupAudit.warnings == 2U);
    CHECK(transport->SentRequests().size() == 81U);
    CheckStartupAuditWireRequests(*transport);
}

TEST_CASE("the startup audit requires an open ownership session")
{
    using namespace fidget;
    using namespace fidget::test;

    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(std::move(ownedTransport), std::chrono::hours(1));
    UseProject(service);

    service.Submit(RunStartupAuditCommand{});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.startupAudit.state == StartupAuditState::Failed;
    }));

    CHECK(service.CurrentSnapshot()->startupAudit.message ==
          "Open a tuner session before auditing module-wide startup "
          "settings.");
    CHECK(transport->SentRequests().empty());
}
