#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/ActivityLogFile.h"
#include "core/ApplicationStorage.h"
#include "core/RecoveryJournal.h"
#include "core/VmeProtocol.h"
#include "fake_transport_factory.h"
#include "hardware/OwnershipService.h"
#include "hardware/TargetProbeOperation.h"
#include "vme_test_support.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace {

using namespace std::chrono_literals;

constexpr std::uint32_t TargetBase = 0x11000000U;

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        const auto unique = std::chrono::steady_clock::now()
                                .time_since_epoch().count();
        for (std::size_t index = 0U; index < 1000U; ++index)
        {
            const auto candidate = std::filesystem::temp_directory_path()
                / ("fidget-coordinator-tests-"
                   + std::to_string(unique) + '-'
                   + std::to_string(index));
            std::error_code error;
            if (std::filesystem::create_directory(candidate, error))
            {
                path_ = std::filesystem::weakly_canonical(candidate);
                break;
            }
            REQUIRE_FALSE(error);
        }
        REQUIRE_FALSE(path_.empty());
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& Get() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

class TemporaryLegacyProject
{
public:
    explicit TemporaryLegacyProject(
        const std::filesystem::path& directory)
        : projectPath_((directory / "legacy-project.mwwcrate").string())
        , recoveryPath_(
              fidget::ProjectTunerRecoveryJournalPath(projectPath_))
        , activityPath_(fidget::ProjectActivityLogPath(projectPath_))
    {
    }

    ~TemporaryLegacyProject()
    {
        std::error_code error;
        std::filesystem::remove(projectPath_, error);
        std::filesystem::remove(recoveryPath_, error);
        std::filesystem::remove(activityPath_, error);
    }

    [[nodiscard]] const std::string& ProjectPath() const
    {
        return projectPath_;
    }

    [[nodiscard]] const std::string& RecoveryPath() const
    {
        return recoveryPath_;
    }

    [[nodiscard]] const std::string& ActivityPath() const
    {
        return activityPath_;
    }

private:
    std::string projectPath_;
    std::string recoveryPath_;
    std::string activityPath_;
};

struct ServiceFixture
{
    ServiceFixture()
    {
        auto transportOwner =
            std::make_unique<fidget::test::FakeCommandTransport>();
        transport = transportOwner.get();
        auto factoryOwner =
            std::make_unique<fidget::test::FakeTransportFactory>(
                std::move(transportOwner));
        factory = factoryOwner.get();
        storage = fidget::ApplicationStoragePathsForHome(home.Get());
        service = std::make_unique<fidget::OwnershipService>(
            std::move(factoryOwner), 1h, storage);
    }

    TemporaryDirectory home;
    fidget::ApplicationStoragePaths storage;
    fidget::test::FakeCommandTransport* transport = nullptr;
    fidget::test::FakeTransportFactory* factory = nullptr;
    std::unique_ptr<fidget::OwnershipService> service;
};

bool WaitFor(
    fidget::OwnershipService& service,
    const std::function<bool(const fidget::TunerSnapshot&)>& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate(*service.CurrentSnapshot()))
            return true;
        std::this_thread::sleep_for(2ms);
    }
    return false;
}

fidget::TunerTargetInput TargetInput(
    const fidget::TunerTargetEndpointKind endpointKind =
        fidget::TunerTargetEndpointKind::SshBridge)
{
    fidget::TunerTargetInput input;
    input.endpointKind = endpointKind;
    input.mvlcHost = "mvlc-test";
    input.mvlcCommandPort = 32768U;
    input.moduleAddress = "1100";
    input.sshDestination = "bridge-test";
    input.remoteBridgeCommand = "fidget_bridge";
    return input;
}

void EditAndSelect(
    fidget::OwnershipService& service,
    const fidget::TunerTargetInput& input)
{
    service.Submit(fidget::EditTunerTargetCommand{input});
    REQUIRE(WaitFor(service, [&input](const fidget::TunerSnapshot& snapshot) {
        return snapshot.target.input == input
            && snapshot.target.verification.invalidated;
    }));

    service.Submit(fidget::SelectTunerTargetCommand{});
    REQUIRE(WaitFor(service, [](const fidget::TunerSnapshot& snapshot) {
        return snapshot.target.selection.has_value();
    }));
    CHECK(service.CurrentSnapshot()
              ->target.selection->moduleAddress.FullA32Value()
          == TargetBase);
}

void QueueMvlcProbeRead(
    fidget::test::FakeCommandTransport& transport,
    const std::uint32_t daqMode = 0U)
{
    using namespace fidget;
    using namespace fidget::test;

    constexpr std::uint16_t Reference = 1U;
    const auto request = BuildMvlcLocalRegisterBatchReadRequest(
        Reference,
        TargetProbeMvlcRegisterOrder.data(),
        TargetProbeMvlcRegisterOrder.size());
    REQUIRE(request.success);
    const std::array<std::uint32_t, 3U> values{{
        TargetProbeExpectedMvlcHardwareId,
        TargetProbeExpectedMvlcFirmware,
        daqMode,
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
        frame.push_back(values[index]);
    }
    transport.QueueExchange({
        EncodeWords(request.words),
        {FakeReceiveAction::Datagram(MakeCommandPacket({frame}))},
    });
}

void QueueSuccessfulTargetProbe(
    fidget::test::FakeCommandTransport& transport)
{
    using namespace fidget;
    using namespace fidget::test;

    QueueMvlcProbeRead(transport);
    TransactionReferences references{2U, 1U};
    QueueRead(
        transport,
        references,
        TargetBase + TargetProbeMdppRegisterOrder[0U],
        Mdpp32HardwareId);
    QueueRead(
        transport,
        references,
        TargetBase + TargetProbeMdppRegisterOrder[1U],
        Mdpp32ScpFirmwareRevisionFw2051);
    QueueRead(
        transport,
        references,
        TargetBase + TargetProbeMdppRegisterOrder[2U],
        0U);
}

void QueueLegacyRead(
    fidget::test::FakeCommandTransport& transport,
    const std::uint16_t address,
    const std::uint16_t reference,
    const std::uint32_t value)
{
    using namespace fidget;
    using namespace fidget::test;

    const auto request = BuildMvlcLocalRegisterReadRequest(
        reference, address);
    transport.QueueExchange({
        EncodeMvlcWordsLittleEndian(request.data(), request.size()),
        {FakeReceiveAction::Datagram(MakeCommandPacket({{
            (static_cast<std::uint32_t>(MvlcSuperFrameType) << 24U) | 3U,
            MvlcReferenceWordCommand | reference,
            MvlcReadLocalCommand | address,
            value,
        }}))},
    });
}

fidget::CrateProject LegacyProject()
{
    fidget::CrateProject project;
    project.mvlcHost = "mvlc-test";
    project.mvlcCommandPort = 32768U;
    project.streamHost = "stream-test";
    project.streamPort = 42333U;
    project.modules.push_back({
        "MDPP-32 SCP",
        TargetBase,
        fidget::MdppBackend::Scp,
        "module-profile.mwwscp",
    });
    return project;
}

void WriteText(const std::string& path, const std::string& text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output << text;
    output.close();
    REQUIRE(output.good());
}

} // namespace

TEST_CASE("direct-target commands dispatch through the coordinator")
{
    using namespace fidget;

    ServiceFixture fixture;
    const auto input = TargetInput();
    EditAndSelect(*fixture.service, input);
    REQUIRE(fixture.service->CurrentSnapshot()->target.selection.has_value());
    CHECK(fixture.service->CurrentSnapshot()->target.selection->input == input);

    std::thread::id probeThread;
    fixture.transport->SetSendHook(
        [&probeThread](const std::vector<std::byte>&) {
            probeThread = std::this_thread::get_id();
        });
    QueueSuccessfulTargetProbe(*fixture.transport);
    const auto beforeProbe = fixture.service->CurrentSnapshot()->revision;
    const auto submittingThread = std::this_thread::get_id();
    fixture.service->Submit(ProbeTunerTargetCommand{});
    REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& snapshot) {
        return snapshot.target.verification.result.outcome
            == TargetProbeOutcome::VerifiedIdle;
    }));

    auto snapshot = fixture.service->CurrentSnapshot();
    CHECK(snapshot->revision == beforeProbe + 2U);
    CHECK(TargetVerificationIsFresh(snapshot->target));
    CHECK(snapshot->tuningSession.evidence.connectionVerificationFresh);
    CHECK(probeThread != std::thread::id{});
    CHECK(probeThread != submittingThread);
    CHECK_FALSE(fixture.transport->IsOpen());
    REQUIRE(fixture.factory->CreateCount() == 1U);
    const auto requests = fixture.factory->Requests();
    REQUIRE(requests.size() == 1U);
    const auto& endpoint = std::get<SshBridgeEndpointRequest>(requests[0U]);
    CHECK(endpoint.mvlcHost == input.mvlcHost);
    CHECK(endpoint.mvlcCommandPort == input.mvlcCommandPort);
    CHECK(endpoint.sshDestination == input.sshDestination);
    CHECK(endpoint.remoteBridgeCommand == input.remoteBridgeCommand);

    auto edited = input;
    edited.mvlcHost = "replacement-controller";
    fixture.service->Submit(EditTunerTargetCommand{edited});
    REQUIRE(WaitFor(*fixture.service, [&edited](const TunerSnapshot& value) {
        return value.target.input == edited
            && value.target.verification.invalidated;
    }));
    snapshot = fixture.service->CurrentSnapshot();
    CHECK_FALSE(TargetProbeEvidenceIsCurrent(snapshot->target));
    CHECK_FALSE(snapshot->tuningSession.evidence.connectionVerificationFresh);
    CHECK(snapshot->target.sessionGate.outcome
          == TunerTargetSessionGateOutcome::NotRequested);

    fixture.service->Submit(ClearTunerTargetCommand{});
    REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& value) {
        return value.target.input.mvlcHost.empty()
            && !value.target.selection.has_value();
    }));
    snapshot = fixture.service->CurrentSnapshot();
    CHECK_FALSE(snapshot->target.verification.invalidated);
    CHECK(snapshot->target.verification.result.outcome
          == TargetProbeOutcome::NotRun);
}

TEST_CASE("clearing a target cancels a probe on the command worker")
{
    using namespace fidget;

    ServiceFixture fixture;
    EditAndSelect(*fixture.service, TargetInput(
        TunerTargetEndpointKind::DirectEthernet));
    QueueMvlcProbeRead(*fixture.transport);

    std::mutex mutex;
    std::condition_variable condition;
    bool receiveEntered = false;
    bool releaseReceive = false;
    fixture.transport->SetReceiveHook(
        [&](const std::size_t) {
            std::unique_lock<std::mutex> lock(mutex);
            receiveEntered = true;
            condition.notify_all();
            condition.wait(lock, [&] { return releaseReceive; });
        });

    fixture.service->Submit(ProbeTunerTargetCommand{});
    bool entered = false;
    {
        std::unique_lock<std::mutex> lock(mutex);
        entered = condition.wait_for(
            lock, 2s, [&] { return receiveEntered; });
    }
    if (!entered)
    {
        const std::lock_guard<std::mutex> lock(mutex);
        releaseReceive = true;
        condition.notify_all();
    }
    REQUIRE(entered);

    fixture.service->Submit(ClearTunerTargetCommand{});
    {
        const std::lock_guard<std::mutex> lock(mutex);
        releaseReceive = true;
    }
    condition.notify_all();

    REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& snapshot) {
        return snapshot.target.input.mvlcHost.empty()
            && !snapshot.statusMessages.empty()
            && snapshot.statusMessages.front().summary.find("cancelled")
                != std::string::npos;
    }));
    CHECK(fixture.transport->SentRequests().size() == 1U);
    CHECK_FALSE(fixture.transport->IsOpen());
}

TEST_CASE("opening a target session is an evidence-only gate")
{
    using namespace fidget;

    ServiceFixture fixture;
    CHECK_FALSE(std::filesystem::exists(fixture.storage.stateDirectory));

    fixture.service->Submit(OpenTunerTargetSessionCommand{});
    REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& snapshot) {
        return snapshot.target.sessionGate.outcome
            == TunerTargetSessionGateOutcome::RefusedVerificationNotFresh;
    }));
    auto snapshot = fixture.service->CurrentSnapshot();
    CHECK_FALSE(snapshot->target.sessionGate.message.empty());
    CHECK(snapshot->target.sessionGate.activityLogPath.empty());
    CHECK(snapshot->target.sessionGate.recoveryJournalPath.empty());
    CHECK(fixture.factory->CreateCount() == 0U);
    CHECK_FALSE(std::filesystem::exists(fixture.storage.stateDirectory));

    const auto input = TargetInput();
    EditAndSelect(*fixture.service, input);
    QueueSuccessfulTargetProbe(*fixture.transport);
    fixture.service->Submit(ProbeTunerTargetCommand{});
    REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& value) {
        return TargetVerificationIsFresh(value.target);
    }));
    const auto requestCount = fixture.factory->CreateCount();
    const auto wireCount = fixture.transport->SentRequests().size();

    fixture.service->Submit(OpenTunerTargetSessionCommand{});
    REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& value) {
        return value.target.sessionGate.outcome
            == TunerTargetSessionGateOutcome::ReadyForPreparation;
    }));
    snapshot = fixture.service->CurrentSnapshot();
    const auto activityPath = std::filesystem::path(
        snapshot->target.sessionGate.activityLogPath);
    const auto recoveryPath = std::filesystem::path(
        snapshot->target.sessionGate.recoveryJournalPath);
    CHECK(activityPath.parent_path() == fixture.storage.logsDirectory);
    CHECK(recoveryPath.parent_path() == fixture.storage.recoveryDirectory);
    CHECK(activityPath.extension() == ".activity");
    CHECK(recoveryPath.extension() == ".recovery");
    CHECK_FALSE(std::filesystem::exists(activityPath));
    CHECK_FALSE(std::filesystem::exists(recoveryPath));
    CHECK(fixture.factory->CreateCount() == requestCount);
    CHECK(fixture.transport->SentRequests().size() == wireCount);
    CHECK(snapshot->tuningSession.phase == TuningSessionPhase::Home);
    CHECK_FALSE(snapshot->tuningSession.evidence.controlHeld);

    auto edited = input;
    edited.remoteBridgeCommand = "alternate-fidget-bridge";
    fixture.service->Submit(EditTunerTargetCommand{edited});
    REQUIRE(WaitFor(*fixture.service, [&edited](const TunerSnapshot& value) {
        return value.target.input == edited
            && value.target.sessionGate.outcome
                == TunerTargetSessionGateOutcome::NotRequested;
    }));
    fixture.service->Submit(OpenTunerTargetSessionCommand{});
    REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& value) {
        return value.target.sessionGate.outcome
            == TunerTargetSessionGateOutcome::RefusedVerificationNotFresh;
    }));
    snapshot = fixture.service->CurrentSnapshot();
    CHECK(snapshot->target.verification.invalidated);
    CHECK(snapshot->target.sessionGate.activityLogPath.empty());
    CHECK(snapshot->target.sessionGate.recoveryJournalPath.empty());
    CHECK(fixture.factory->CreateCount() == requestCount);
    CHECK(fixture.transport->SentRequests().size() == wireCount);
}

TEST_CASE("legacy project dispatch and adjacent recovery discovery are unchanged")
{
    using namespace fidget;

    SUBCASE("legacy status dispatch still uses the project factory path")
    {
        ServiceFixture fixture;
        TemporaryLegacyProject files(fixture.home.Get());
        fixture.service->Submit(UseCrateProjectCommand{
            files.ProjectPath(), LegacyProject(), 0U});
        REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& snapshot) {
            return snapshot.projectActive;
        }));

        QueueLegacyRead(
            *fixture.transport, FirmwareRevisionRegister, 1U, 0x0046U);
        QueueLegacyRead(*fixture.transport, DaqModeRegister, 2U, 0U);
        QueueLegacyRead(
            *fixture.transport,
            HardwareIdRegister,
            3U,
            ExpectedMvlcHardwareId);
        fixture.service->Submit(CheckStatusCommand{});
        REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& snapshot) {
            return snapshot.ownership == GuidedTunerOwnershipState::Idle;
        }));

        const auto snapshot = fixture.service->CurrentSnapshot();
        CHECK(snapshot->recoveryJournalPath == files.RecoveryPath());
        CHECK(snapshot->activityLogPath == files.ActivityPath());
        CHECK(snapshot->target.verification.result.outcome
              == TargetProbeOutcome::NotRun);
        REQUIRE(fixture.factory->Projects().size() == 1U);
        CHECK(fixture.factory->CreateCount() == 1U);
    }

    SUBCASE("legacy project-adjacent recovery evidence is still discovered")
    {
        ServiceFixture fixture;
        TemporaryLegacyProject files(fixture.home.Get());
        WriteText(files.RecoveryPath(), "malformed recovery evidence\n");
        fixture.service->Submit(UseCrateProjectCommand{
            files.ProjectPath(), LegacyProject(), 0U});
        REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& snapshot) {
            return snapshot.recoveryJournalStatus
                == RecoveryJournalStatus::Malformed;
        }));

        const auto snapshot = fixture.service->CurrentSnapshot();
        CHECK(snapshot->projectActive);
        CHECK(snapshot->ownership
              == GuidedTunerOwnershipState::RecoveryRequired);
        CHECK(snapshot->recoveryJournalPath == files.RecoveryPath());
        CHECK(fixture.factory->CreateCount() == 0U);
        CHECK(fixture.factory->Projects().empty());
    }
}
