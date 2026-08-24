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

#include <algorithm>
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
#include <sstream>
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

fidget::TunerRecoveryRecord ApplicationRecoveryRecord(
    fidget::ControllerEndpointRequest endpoint = {
        fidget::ControllerEndpointKind::DirectEthernet,
        "controller-test",
        32768U,
        {},
        {},
    })
{
    using namespace fidget;

    TunerRecoveryRecord record;
    record.formatVersion = TunerRecoveryJournalV5FormatVersion;
    TunerRecoveryV5Data data;
    data.sessionPhase = TuningSessionPhase::Preparing;
    data.endpoint = std::move(endpoint);
    data.identity.mvlcHardwareId = 0x5008U;
    data.identity.mvlcFirmwareRevision = 0x0046U;
    data.identity.targetBaseAddress = TargetBase;
    data.identity.targetHardwareId = Mdpp32HardwareId;
    data.identity.targetFirmwareRevision =
        Mdpp32ScpFirmwareRevisionFw2051;
    data.selectorParkingRequired = true;
    record.version5 = std::move(data);
    return record;
}

void EditTarget(
    fidget::OwnershipService& service,
    const fidget::TunerTargetInput& input)
{
    service.Submit(fidget::EditTunerTargetCommand{input});
    REQUIRE(WaitFor(service, [&input](const fidget::TunerSnapshot& snapshot) {
        return snapshot.target.input == input
            && snapshot.target.verification.invalidated
            && snapshot.target.controllerVerification.invalidated;
    }));
}

void QueueMvlcProbeRead(
    fidget::test::FakeCommandTransport& transport,
    const std::uint32_t daqMode = 0U,
    const std::uint32_t hardwareId =
        fidget::TargetProbeExpectedMvlcHardwareId,
    const std::uint32_t firmware =
        fidget::TargetProbeExpectedMvlcFirmware)
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
        hardwareId,
        firmware,
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

void QueueSuccessfulTargetCheck(
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

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    std::ostringstream contents;
    contents << input.rdbuf();
    REQUIRE_FALSE(input.bad());
    return contents.str();
}

std::filesystem::path FixturePath(const std::string& name)
{
    return std::filesystem::path(FIDGET_TEST_FIXTURE_DIR) / name;
}

void WriteWorkspaceWithTargetScript(
    const std::filesystem::path& path,
    const std::string& scriptText)
{
    auto document = nlohmann::ordered_json::parse(
        ReadText(FixturePath("mvme_workspace_v4.vme")));
    document["DAQConfig"]["events"][0]["modules"][0]["initScripts"] =
        nlohmann::ordered_json::array({{
            {"id", "script_workspace_coordinator_fixture"},
            {"name", "workspace coordinator fixture"},
            {"enabled", true},
            {"vme_script", scriptText},
        }});
    WriteText(path.string(), document.dump(2));
}

} // namespace

TEST_CASE("direct-target commands dispatch through the coordinator")
{
    using namespace fidget;

    ServiceFixture fixture;
    const auto input = TargetInput();
    fixture.service->Submit(EditTunerTargetCommand{input});
    REQUIRE(WaitFor(*fixture.service, [&input](const TunerSnapshot& snapshot) {
        return snapshot.target.input == input
            && snapshot.target.verification.invalidated
            && snapshot.target.controllerVerification.invalidated;
    }));
    CHECK(fixture.service->CurrentSnapshot()
              ->tuningSession.evidence.endpointInputsValid);
    CHECK(fixture.service->CurrentSnapshot()->applicationRecovery.state
          == ApplicationRecoveryDiscoveryState::Empty);
    CHECK(fixture.service->CurrentSnapshot()
              ->tuningSession.evidence.noRecoveryPending);
    CHECK_FALSE(fixture.service->CurrentSnapshot()
                    ->tuningSession.evidence.currentConnectionRequestValid);

    QueueMvlcProbeRead(*fixture.transport);
    const auto beforeConnect = fixture.service->CurrentSnapshot()->revision;
    const auto submittingThread = std::this_thread::get_id();
    std::thread::id probeThread;
    fixture.transport->SetSendHook(
        [&probeThread](const std::vector<std::byte>&) {
            probeThread = std::this_thread::get_id();
        });
    fixture.service->Submit(SelectTunerTargetCommand{});
    REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& snapshot) {
        return snapshot.target.controllerVerification.result.outcome
            == ControllerProbeOutcome::VerifiedIdle;
    }));

    auto snapshot = fixture.service->CurrentSnapshot();
    CHECK(snapshot->revision == beforeConnect + 2U);
    REQUIRE(snapshot->target.selection.has_value());
    CHECK(snapshot->target.selection->input == input);
    CHECK(snapshot->target.selection->moduleAddress.FullA32Value()
          == TargetBase);
    CHECK(ControllerVerificationIsFresh(snapshot->target));
    CHECK_FALSE(TargetVerificationIsFresh(snapshot->target));
    CHECK(snapshot->tuningSession.evidence.controllerConnected);
    CHECK(snapshot->tuningSession.evidence.controllerVerificationFresh);
    CHECK_FALSE(snapshot->tuningSession.evidence.targetVerificationFresh);
    CHECK_FALSE(snapshot->tuningSession.evidence.connectionVerificationFresh);
    CHECK(probeThread != std::thread::id{});
    CHECK(probeThread != submittingThread);
    CHECK_FALSE(fixture.transport->IsOpen());
    CHECK(fixture.transport->SentRequests().size() == 1U);
    CHECK(DecodeWireOperations(*fixture.transport).empty());
    REQUIRE(fixture.factory->CreateCount() == 1U);

    const auto connectionPreferences = LoadApplicationPreferences(
        fixture.storage);
    INFO(connectionPreferences.message);
    REQUIRE(connectionPreferences.success);
    CHECK(connectionPreferences.preferences.lastVerifiedEthernetHost.empty());
    CHECK(connectionPreferences.preferences.lastVerifiedModuleAddress.empty());
    CHECK(connectionPreferences.preferences.sshDestination
          == input.sshDestination);
    CHECK(connectionPreferences.preferences.remoteBridgeCommand
          == input.remoteBridgeCommand);

    QueueSuccessfulTargetCheck(*fixture.transport);
    const auto beforeProbe = fixture.service->CurrentSnapshot()->revision;
    fixture.service->Submit(ProbeTunerTargetCommand{});
    REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& snapshot) {
        return snapshot.target.verification.result.outcome
            == TargetProbeOutcome::VerifiedIdle;
    }));

    snapshot = fixture.service->CurrentSnapshot();
    CHECK(snapshot->revision == beforeProbe + 2U);
    CHECK(TargetVerificationIsFresh(snapshot->target));
    CHECK(snapshot->tuningSession.evidence.connectionVerificationFresh);
    CHECK(probeThread != std::thread::id{});
    CHECK(probeThread != submittingThread);
    CHECK_FALSE(fixture.transport->IsOpen());
    REQUIRE(fixture.factory->CreateCount() == 2U);
    const auto operations = DecodeWireOperations(*fixture.transport);
    REQUIRE(operations.size() == 3U);
    CHECK(std::none_of(
        operations.begin(), operations.end(),
        [](const auto& operation) { return operation.write; }));
    CHECK(fixture.transport->SentRequests().size() == 8U);
    const auto requests = fixture.factory->Requests();
    REQUIRE(requests.size() == 2U);
    for (const auto& request : requests)
    {
        const auto& endpoint = std::get<SshBridgeEndpointRequest>(request);
        CHECK(endpoint.mvlcHost == input.mvlcHost);
        CHECK(endpoint.mvlcCommandPort == input.mvlcCommandPort);
        CHECK(endpoint.sshDestination == input.sshDestination);
        CHECK(endpoint.remoteBridgeCommand == input.remoteBridgeCommand);
    }

    const auto savedPreferences = LoadApplicationPreferences(fixture.storage);
    INFO(savedPreferences.message);
    REQUIRE(savedPreferences.success);
    CHECK(savedPreferences.preferences.lastVerifiedEthernetHost
          == input.mvlcHost);
    CHECK(savedPreferences.preferences.lastVerifiedModuleAddress
          == input.moduleAddress);
    CHECK(savedPreferences.preferences.sshDestination
          == input.sshDestination);
    CHECK(savedPreferences.preferences.remoteBridgeCommand
          == input.remoteBridgeCommand);

    auto moduleEdited = input;
    moduleEdited.moduleAddress = "0x2200";
    fixture.service->Submit(EditTunerTargetCommand{moduleEdited});
    REQUIRE(WaitFor(*fixture.service, [&moduleEdited](const TunerSnapshot& value) {
        return value.target.input == moduleEdited
            && value.target.verification.invalidated
            && !value.target.controllerVerification.invalidated;
    }));
    snapshot = fixture.service->CurrentSnapshot();
    CHECK(ControllerVerificationIsFresh(snapshot->target));
    CHECK_FALSE(TargetProbeEvidenceIsCurrent(snapshot->target));
    CHECK(snapshot->tuningSession.evidence.controllerConnected);
    CHECK(snapshot->tuningSession.evidence.controllerVerificationFresh);
    CHECK_FALSE(snapshot->tuningSession.evidence.connectionVerificationFresh);
    CHECK(snapshot->tuningSession.evidence.targetModuleAddressValid);
    CHECK(snapshot->target.sessionGate.outcome
          == TunerTargetSessionGateOutcome::NotRequested);

    auto endpointEdited = moduleEdited;
    endpointEdited.mvlcHost = "replacement-controller";
    fixture.service->Submit(EditTunerTargetCommand{endpointEdited});
    REQUIRE(WaitFor(*fixture.service, [&endpointEdited](
        const TunerSnapshot& value) {
        return value.target.input == endpointEdited
            && value.target.verification.invalidated
            && value.target.controllerVerification.invalidated;
    }));
    snapshot = fixture.service->CurrentSnapshot();
    CHECK_FALSE(ControllerVerificationIsFresh(snapshot->target));
    CHECK_FALSE(snapshot->tuningSession.evidence.controllerConnected);
    CHECK_FALSE(snapshot->tuningSession.evidence.controllerVerificationFresh);
    const auto preferencesAfterEdit = LoadApplicationPreferences(
        fixture.storage);
    REQUIRE(preferencesAfterEdit.success);
    CHECK(preferencesAfterEdit.preferences.lastVerifiedEthernetHost
          == input.mvlcHost);
    CHECK(preferencesAfterEdit.preferences.remoteBridgeCommand
          == input.remoteBridgeCommand);

    fixture.service->Submit(ClearTunerTargetCommand{});
    REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& value) {
        return value.target.input.mvlcHost.empty()
            && !value.target.selection.has_value();
    }));
    snapshot = fixture.service->CurrentSnapshot();
    CHECK_FALSE(snapshot->target.verification.invalidated);
    CHECK_FALSE(snapshot->target.controllerVerification.invalidated);
    CHECK(snapshot->target.verification.result.outcome
          == TargetProbeOutcome::NotRun);
    CHECK(snapshot->target.controllerVerification.result.outcome
          == ControllerProbeOutcome::NotRun);
}

TEST_CASE("Check refuses before Connect without transport traffic")
{
    using namespace fidget;

    ServiceFixture fixture;
    EditTarget(*fixture.service, TargetInput(
        TunerTargetEndpointKind::SshBridge));
    fixture.service->Submit(ProbeTunerTargetCommand{});
    REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& snapshot) {
        return snapshot.target.verification.result.message
            == "The target check requires a current verified controller "
               "connection.";
    }));

    const auto snapshot = fixture.service->CurrentSnapshot();
    CHECK(snapshot->target.verification.result.outcome
          == TargetProbeOutcome::NotRun);
    CHECK(snapshot->statusMessages.front().summary
          == "The read-only target check was not started.");
    CHECK(snapshot->statusMessages.front().detail
          == "Run Connect successfully for the current endpoint first.");
    CHECK(fixture.factory->CreateCount() == 0U);
    CHECK(fixture.transport->SentRequests().empty());
}

TEST_CASE("Connect ignores an invalid module address and Check refuses it")
{
    using namespace fidget;

    ServiceFixture fixture;
    auto input = TargetInput(TunerTargetEndpointKind::DirectEthernet);
    input.moduleAddress = "not-an-address";
    EditTarget(*fixture.service, input);

    auto snapshot = fixture.service->CurrentSnapshot();
    CHECK(snapshot->tuningSession.evidence.endpointInputsValid);
    CHECK_FALSE(snapshot->tuningSession.evidence.targetModuleAddressValid);

    QueueMvlcProbeRead(*fixture.transport);
    fixture.service->Submit(SelectTunerTargetCommand{});
    REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& snapshot) {
        return snapshot.target.controllerVerification.result.outcome
            == ControllerProbeOutcome::VerifiedIdle;
    }));
    snapshot = fixture.service->CurrentSnapshot();
    CHECK(ControllerVerificationIsFresh(snapshot->target));
    CHECK_FALSE(snapshot->target.selection.has_value());
    CHECK(fixture.transport->SentRequests().size() == 1U);
    CHECK(DecodeWireOperations(*fixture.transport).empty());
    CHECK(snapshot->tuningSession.evidence.controllerVerificationFresh);
    CHECK_FALSE(snapshot->tuningSession.evidence.targetVerificationFresh);

    const auto createCount = fixture.factory->CreateCount();
    const auto wireCount = fixture.transport->SentRequests().size();
    fixture.service->Submit(ProbeTunerTargetCommand{});
    REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& value) {
        return value.target.verification.result.message
            == "The target check requires a valid module address.";
    }));
    snapshot = fixture.service->CurrentSnapshot();
    CHECK(snapshot->statusMessages.front().summary
          == "The read-only target check was not started.");
    CHECK(snapshot->statusMessages.front().detail
          == "The target-module address is not valid hexadecimal.");
    CHECK(ControllerVerificationIsFresh(snapshot->target));
    CHECK(fixture.factory->CreateCount() == createCount);
    CHECK(fixture.transport->SentRequests().size() == wireCount);

    input.moduleAddress = "0x1100";
    fixture.service->Submit(EditTunerTargetCommand{input});
    REQUIRE(WaitFor(*fixture.service, [&input](const TunerSnapshot& value) {
        return value.target.input == input
            && value.target.verification.invalidated;
    }));
    snapshot = fixture.service->CurrentSnapshot();
    CHECK(ControllerVerificationIsFresh(snapshot->target));
    CHECK(snapshot->tuningSession.evidence.controllerVerificationFresh);
    CHECK(snapshot->tuningSession.evidence.targetModuleAddressValid);
}

TEST_CASE("Check invalidates old controller evidence when revalidation fails")
{
    using namespace fidget;

    SUBCASE("DAQ became active")
    {
        ServiceFixture fixture;
        EditTarget(*fixture.service, TargetInput(
            TunerTargetEndpointKind::DirectEthernet));
        QueueMvlcProbeRead(*fixture.transport);
        fixture.service->Submit(SelectTunerTargetCommand{});
        REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& value) {
            return ControllerVerificationIsFresh(value.target);
        }));

        QueueMvlcProbeRead(*fixture.transport, 0x0005U);
        fixture.service->Submit(ProbeTunerTargetCommand{});
        REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& value) {
            return value.target.verification.result.outcome
                == TargetProbeOutcome::ControllerDaqActive;
        }));
        const auto snapshot = fixture.service->CurrentSnapshot();
        CHECK(snapshot->target.controllerVerification.invalidated);
        CHECK_FALSE(ControllerVerificationIsFresh(snapshot->target));
        CHECK_FALSE(TargetVerificationIsFresh(snapshot->target));
        CHECK(fixture.factory->CreateCount() == 2U);
        CHECK(fixture.transport->SentRequests().size() == 2U);
        CHECK(DecodeWireOperations(*fixture.transport).empty());
        CHECK(snapshot->tuningSession.evidence.activeControllerUseDetected);
        CHECK_FALSE(
            snapshot->tuningSession.evidence.connectionVerificationFresh);
    }

    SUBCASE("controller identity changed")
    {
        ServiceFixture fixture;
        EditTarget(*fixture.service, TargetInput(
            TunerTargetEndpointKind::DirectEthernet));
        QueueMvlcProbeRead(*fixture.transport);
        fixture.service->Submit(SelectTunerTargetCommand{});
        REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& value) {
            return ControllerVerificationIsFresh(value.target);
        }));

        QueueMvlcProbeRead(
            *fixture.transport, 0U, 0x1234U);
        fixture.service->Submit(ProbeTunerTargetCommand{});
        REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& value) {
            return value.target.verification.result.outcome
                == TargetProbeOutcome::WrongMvlcIdentity;
        }));
        const auto snapshot = fixture.service->CurrentSnapshot();
        CHECK(snapshot->target.controllerVerification.invalidated);
        CHECK_FALSE(ControllerVerificationIsFresh(snapshot->target));
        CHECK_FALSE(TargetVerificationIsFresh(snapshot->target));
        CHECK(fixture.transport->SentRequests().size() == 2U);
        CHECK(DecodeWireOperations(*fixture.transport).empty());
        CHECK_FALSE(
            snapshot->tuningSession.evidence.connectionVerificationFresh);
    }

    SUBCASE("controller firmware changed")
    {
        ServiceFixture fixture;
        EditTarget(*fixture.service, TargetInput(
            TunerTargetEndpointKind::DirectEthernet));
        QueueMvlcProbeRead(*fixture.transport);
        fixture.service->Submit(SelectTunerTargetCommand{});
        REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& value) {
            return ControllerVerificationIsFresh(value.target);
        }));

        QueueMvlcProbeRead(
            *fixture.transport,
            0U,
            TargetProbeExpectedMvlcHardwareId,
            0x0045U);
        fixture.service->Submit(ProbeTunerTargetCommand{});
        REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& value) {
            return value.target.verification.result.outcome
                == TargetProbeOutcome::WrongMvlcFirmware;
        }));
        const auto snapshot = fixture.service->CurrentSnapshot();
        CHECK(snapshot->target.controllerVerification.invalidated);
        CHECK_FALSE(ControllerVerificationIsFresh(snapshot->target));
        CHECK_FALSE(TargetVerificationIsFresh(snapshot->target));
        CHECK(fixture.transport->SentRequests().size() == 2U);
        CHECK(DecodeWireOperations(*fixture.transport).empty());
        CHECK_FALSE(
            snapshot->tuningSession.evidence.connectionVerificationFresh);
    }
}

TEST_CASE("SSH convenience persistence never downgrades a probe error")
{
    using namespace fidget;

    ServiceFixture fixture;
    const auto storageReady = EnsureApplicationStorageDirectories(
        fixture.storage);
    REQUIRE(storageReady.success);
    REQUIRE(std::filesystem::create_directory(
        fixture.storage.preferencesFile));
    EditTarget(*fixture.service, TargetInput(
        TunerTargetEndpointKind::SshBridge));

    SUBCASE("successful probe is elevated to warning")
    {
        QueueMvlcProbeRead(*fixture.transport);
        fixture.service->Submit(SelectTunerTargetCommand{});
        REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& value) {
            return value.target.controllerVerification.result.outcome
                == ControllerProbeOutcome::VerifiedIdle;
        }));
        const auto snapshot = fixture.service->CurrentSnapshot();
        CHECK(snapshot->statusMessages.front().level
              == TunerStatusLevel::Warning);
    }

    SUBCASE("failed probe remains error")
    {
        QueueMvlcProbeRead(*fixture.transport, 0U, 0x1234U);
        fixture.service->Submit(SelectTunerTargetCommand{});
        REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& value) {
            return value.target.controllerVerification.result.outcome
                == ControllerProbeOutcome::WrongMvlcIdentity;
        }));
        const auto snapshot = fixture.service->CurrentSnapshot();
        CHECK(snapshot->statusMessages.front().level
              == TunerStatusLevel::Error);
    }
}

TEST_CASE("clearing a target cancels a probe on the command worker")
{
    using namespace fidget;

    ServiceFixture fixture;
    EditTarget(*fixture.service, TargetInput(
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

    fixture.service->Submit(SelectTunerTargetCommand{});
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

TEST_CASE("workspace commands parse and evaluate passively on the worker")
{
    using namespace fidget;

    ServiceFixture fixture;
    EditTarget(*fixture.service, TargetInput());
    const auto path = FixturePath("mvme_workspace_v4.vme").string();
    const auto before = fixture.service->CurrentSnapshot()->revision;
    fixture.service->Submit(SetTunerWorkspaceCommand{path});
    REQUIRE(WaitFor(*fixture.service, [&path](const TunerSnapshot& snapshot) {
        return snapshot.workspace.sourcePath == path
            && snapshot.workspace.outcome
                == TunerWorkspaceLoadOutcome::Loaded;
    }));

    auto snapshot = fixture.service->CurrentSnapshot();
    CHECK(snapshot->revision == before + 2U);
    CHECK(snapshot->workspace.sourcePath == path);
    REQUIRE(snapshot->workspace.workspace.has_value());
    CHECK(snapshot->workspace.targetLookupPerformed);
    REQUIRE(snapshot->workspace.evaluatedTargetAddress.has_value());
    CHECK(snapshot->workspace.evaluatedTargetAddress->FullA32Value()
          == TargetBase);
    CHECK(snapshot->workspace.targetLookup.status
          == MvmeWorkspaceTargetStatus::Found);
    REQUIRE(snapshot->workspace.targetLookup.target.has_value());
    CHECK(snapshot->workspace.evaluationPerformed);
    CHECK(snapshot->workspace.evaluation.state
          == MvmeInitScriptEvaluationState::
              CompleteWithUnresolvedNonFrontend);
    CHECK_FALSE(snapshot->workspace.warnings.empty());
    CHECK_FALSE(snapshot->workspace.message.empty());
    CHECK(snapshot->target.sessionGate.outcome
          == TunerTargetSessionGateOutcome::NotRequested);
    CHECK(fixture.factory->CreateCount() == 0U);
    CHECK(fixture.transport->SentRequests().empty());
    CHECK_FALSE(snapshot->tuningSession.evidence
                    .liveRestoreSnapshotCaptured);
    CHECK_FALSE(snapshot->tuningSession.evidence.recoveryRecordDurable);
    CHECK_FALSE(snapshot->tuningSession.evidence
                    .workspaceStartingSettingsResolved);
}

TEST_CASE("workspace snapshot exposes conditional and failed evaluations")
{
    using namespace fidget;

    ServiceFixture fixture;
    EditTarget(*fixture.service, TargetInput());
    const auto conditionalPath = fixture.home.Get() / "conditional.vme";
    WriteWorkspaceWithTargetScript(
        conditionalPath,
        "0x6100 0\n"
        "accu_test eq 1 \"live check\"\n"
        "0x6110 24");
    fixture.service->Submit(
        SetTunerWorkspaceCommand{conditionalPath.string()});
    REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& snapshot) {
        return snapshot.workspace.evaluationPerformed
            && snapshot.workspace.evaluation.state
                == MvmeInitScriptEvaluationState::
                    ConditionalAfterAccuTest;
    }));
    auto snapshot = fixture.service->CurrentSnapshot();
    CHECK(snapshot->workspace.evaluation.frontendWrites.empty());
    CHECK_FALSE(snapshot->workspace.warnings.empty());
    CHECK(fixture.factory->CreateCount() == 0U);

    const auto failedPath = fixture.home.Get() / "parse-failed.vme";
    WriteWorkspaceWithTargetScript(
        failedPath,
        "0x6100 0\n"
        "0x6110 20\n"
        "set value ${missing}");
    fixture.service->Submit(SetTunerWorkspaceCommand{failedPath.string()});
    REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& value) {
        return value.workspace.evaluationPerformed
            && value.workspace.evaluation.state
                == MvmeInitScriptEvaluationState::Failed;
    }));
    snapshot = fixture.service->CurrentSnapshot();
    CHECK(snapshot->workspace.evaluation.frontendWrites.empty());
    CHECK(snapshot->workspace.evaluation.finalFrontendValues.empty());
    CHECK_FALSE(snapshot->workspace.warnings.empty());
    CHECK(snapshot->statusMessages.front().level == TunerStatusLevel::Error);
    CHECK(fixture.factory->CreateCount() == 0U);
    CHECK(fixture.transport->SentRequests().empty());
}

TEST_CASE("workspace failures remain visible without hardware access")
{
    using namespace fidget;

    ServiceFixture fixture;
    EditTarget(*fixture.service, TargetInput());
    const auto missing = fixture.home.Get() / "missing.vme";
    fixture.service->Submit(SetTunerWorkspaceCommand{missing.string()});
    REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& snapshot) {
        return snapshot.workspace.outcome
            == TunerWorkspaceLoadOutcome::FileUnavailable;
    }));
    auto snapshot = fixture.service->CurrentSnapshot();
    CHECK(snapshot->workspace.sourcePath == missing.string());
    CHECK_FALSE(snapshot->workspace.workspace.has_value());

    const auto malformed = fixture.home.Get() / "malformed.vme";
    WriteText(malformed.string(), "{ malformed workspace fixture\n");
    fixture.service->Submit(SetTunerWorkspaceCommand{malformed.string()});
    REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& value) {
        return value.workspace.outcome
            == TunerWorkspaceLoadOutcome::ParseFailed;
    }));
    snapshot = fixture.service->CurrentSnapshot();
    CHECK(snapshot->workspace.sourcePath == malformed.string());
    CHECK_FALSE(snapshot->workspace.workspace.has_value());
    CHECK_FALSE(snapshot->workspace.message.empty());
    CHECK(fixture.factory->CreateCount() == 0U);
    CHECK(fixture.transport->SentRequests().empty());
}

TEST_CASE("workspace evidence follows only its target-address dependency")
{
    using namespace fidget;

    ServiceFixture fixture;
    auto input = TargetInput(TunerTargetEndpointKind::DirectEthernet);
    EditTarget(*fixture.service, input);
    const auto path = FixturePath("mvme_workspace_v4.vme").string();
    fixture.service->Submit(SetTunerWorkspaceCommand{path});
    REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& snapshot) {
        return snapshot.workspace.evaluationPerformed;
    }));

    auto endpointEdited = input;
    endpointEdited.mvlcHost = "alternate-controller";
    fixture.service->Submit(EditTunerTargetCommand{endpointEdited});
    REQUIRE(WaitFor(*fixture.service, [&endpointEdited](
        const TunerSnapshot& snapshot) {
        return snapshot.target.input == endpointEdited;
    }));
    auto snapshot = fixture.service->CurrentSnapshot();
    CHECK(snapshot->workspace.evaluationPerformed);
    REQUIRE(snapshot->workspace.evaluatedTargetAddress.has_value());
    CHECK(snapshot->workspace.evaluatedTargetAddress->FullA32Value()
          == TargetBase);

    auto moduleEdited = endpointEdited;
    moduleEdited.moduleAddress = "0x2200";
    fixture.service->Submit(EditTunerTargetCommand{moduleEdited});
    REQUIRE(WaitFor(*fixture.service, [&moduleEdited](
        const TunerSnapshot& value) {
        return value.target.input == moduleEdited
            && value.workspace.targetLookupPerformed
            && value.workspace.targetLookup.status
                == MvmeWorkspaceTargetStatus::NotFound;
    }));
    snapshot = fixture.service->CurrentSnapshot();
    REQUIRE(snapshot->workspace.workspace.has_value());
    CHECK_FALSE(snapshot->workspace.evaluationPerformed);
    CHECK(snapshot->workspace.evaluation.frontendWrites.empty());

    fixture.service->Submit(ClearTunerTargetCommand{});
    REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& value) {
        return value.target.input.mvlcHost.empty()
            && !value.workspace.targetLookupPerformed;
    }));
    snapshot = fixture.service->CurrentSnapshot();
    CHECK(snapshot->workspace.outcome == TunerWorkspaceLoadOutcome::Loaded);
    REQUIRE(snapshot->workspace.workspace.has_value());
    CHECK(snapshot->workspace.sourcePath == path);

    EditTarget(*fixture.service, input);
    REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& value) {
        return value.workspace.evaluationPerformed;
    }));
    QueueMvlcProbeRead(*fixture.transport);
    fixture.service->Submit(SelectTunerTargetCommand{});
    REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& value) {
        return ControllerVerificationIsFresh(value.target);
    }));
    const auto createCount = fixture.factory->CreateCount();
    const auto requestCount = fixture.transport->SentRequests().size();
    fixture.service->Submit(ClearTunerWorkspaceCommand{});
    REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& value) {
        return value.workspace.outcome
            == TunerWorkspaceLoadOutcome::NotSelected;
    }));
    snapshot = fixture.service->CurrentSnapshot();
    CHECK(snapshot->workspace.sourcePath.empty());
    CHECK_FALSE(snapshot->workspace.workspace.has_value());
    CHECK(ControllerVerificationIsFresh(snapshot->target));
    CHECK(fixture.factory->CreateCount() == createCount);
    CHECK(fixture.transport->SentRequests().size() == requestCount);
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
    EditTarget(*fixture.service, input);
    QueueMvlcProbeRead(*fixture.transport);
    fixture.service->Submit(SelectTunerTargetCommand{});
    REQUIRE(WaitFor(*fixture.service, [](const TunerSnapshot& value) {
        return ControllerVerificationIsFresh(value.target);
    }));
    QueueSuccessfulTargetCheck(*fixture.transport);
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
    CHECK(snapshot->target.controllerVerification.invalidated);
    CHECK(snapshot->target.sessionGate.activityLogPath.empty());
    CHECK(snapshot->target.sessionGate.recoveryJournalPath.empty());
    CHECK(fixture.factory->CreateCount() == requestCount);
    CHECK(fixture.transport->SentRequests().size() == wireCount);
}

TEST_CASE("application recovery is discovered before direct-target Home work")
{
    using namespace fidget;
    using namespace fidget::test;

    TemporaryDirectory home;
    const auto storage = ApplicationStoragePathsForHome(home.Get());
    const auto ready = EnsureApplicationStorageDirectories(storage);
    INFO(ready.message);
    REQUIRE(ready.success);
    const auto journal = storage.recoveryDirectory / "pending.recovery";
    const auto saved = SaveTunerRecoveryJournal(
        ApplicationRecoveryRecord(), journal.string());
    INFO(saved.message);
    REQUIRE(saved.success);
    const auto journalBefore = [&journal] {
        std::ifstream input(journal, std::ios::binary);
        REQUIRE(input.good());
        std::ostringstream text;
        text << input.rdbuf();
        REQUIRE_FALSE(input.bad());
        return text.str();
    }();

    auto transportOwner = std::make_unique<FakeCommandTransport>();
    auto* transport = transportOwner.get();
    auto factoryOwner = std::make_unique<FakeTransportFactory>(
        std::move(transportOwner));
    auto* factory = factoryOwner.get();
    OwnershipService service(std::move(factoryOwner), 1h, storage);

    auto snapshot = service.CurrentSnapshot();
    CHECK(snapshot->applicationRecovery.state
          == ApplicationRecoveryDiscoveryState::PendingV5);
    REQUIRE(snapshot->applicationRecovery.record.has_value());
    REQUIRE(snapshot->applicationRecovery.record->version5.has_value());
    CHECK(snapshot->applicationRecovery.record->version5
              ->identity.targetBaseAddress
          == TargetBase);
    CHECK(snapshot->ownership
          == GuidedTunerOwnershipState::RecoveryRequired);
    CHECK_FALSE(snapshot->tuningSession.evidence.noRecoveryPending);
    CHECK(snapshot->tuningSession.evidence.recoveryRecordPresent);
    CHECK(snapshot->tuningSession.evidence.recoveryContextEstablished);
    CHECK_FALSE(snapshot->tuningSession.evidence
                    .recoveryControllerAndTargetIdentitiesVerified);
    CHECK(factory->CreateCount() == 0U);
    CHECK(transport->SentRequests().empty());

    const auto input = TargetInput(TunerTargetEndpointKind::DirectEthernet);
    service.Submit(EditTunerTargetCommand{input});
    REQUIRE(WaitFor(service, [&input](const TunerSnapshot& value) {
        return value.target.input == input;
    }));
    snapshot = service.CurrentSnapshot();
    CHECK(snapshot->applicationRecovery.state
          == ApplicationRecoveryDiscoveryState::PendingV5);
    CHECK_FALSE(snapshot->tuningSession.evidence.noRecoveryPending);

    service.Submit(OpenTunerTargetSessionCommand{});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& value) {
        return value.target.sessionGate.outcome
            == TunerTargetSessionGateOutcome::RefusedRecoveryPending;
    }));
    CHECK(factory->CreateCount() == 0U);
    CHECK(transport->SentRequests().empty());

    std::ifstream afterInput(journal, std::ios::binary);
    REQUIRE(afterInput.good());
    std::ostringstream after;
    after << afterInput.rdbuf();
    REQUIRE_FALSE(afterInput.bad());
    CHECK(after.str() == journalBefore);
}

TEST_CASE("malformed startup recovery blocks without contacting hardware")
{
    using namespace fidget;
    using namespace fidget::test;

    TemporaryDirectory home;
    const auto storage = ApplicationStoragePathsForHome(home.Get());
    REQUIRE(EnsureApplicationStorageDirectories(storage).success);
    const auto journal = storage.recoveryDirectory / "damaged.recovery";
    WriteText(journal.string(), "damaged recovery evidence\n");

    auto transportOwner = std::make_unique<FakeCommandTransport>();
    auto* transport = transportOwner.get();
    auto factoryOwner = std::make_unique<FakeTransportFactory>(
        std::move(transportOwner));
    auto* factory = factoryOwner.get();
    OwnershipService service(std::move(factoryOwner), 1h, storage);

    const auto snapshot = service.CurrentSnapshot();
    CHECK(snapshot->applicationRecovery.state
          == ApplicationRecoveryDiscoveryState::Blocked);
    CHECK(snapshot->applicationRecovery.blockReason
          == ApplicationRecoveryBlockReason::MalformedRecord);
    CHECK(snapshot->applicationRecovery.message.find("malformed")
          != std::string::npos);
    CHECK(snapshot->ownership
          == GuidedTunerOwnershipState::RecoveryRequired);
    CHECK_FALSE(snapshot->tuningSession.evidence.noRecoveryPending);
    CHECK(snapshot->tuningSession.evidence.recoveryRecordPresent);
    CHECK(snapshot->tuningSession.evidence.recoveryRecordRetained);
    CHECK(snapshot->tuningSession.evidence.recoveryComparison
          == TuningRecoveryComparison::InsufficientEvidence);
    CHECK_FALSE(snapshot->tuningSession.evidence.noRecoveryWritesSent);
    CHECK(factory->CreateCount() == 0U);
    CHECK(transport->SentRequests().empty());
    CHECK(std::filesystem::exists(journal));
    CHECK([&journal] {
        std::ifstream input(journal, std::ios::binary);
        std::ostringstream text;
        text << input.rdbuf();
        return text.str();
    }() == "damaged recovery evidence\n");
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
        CHECK(snapshot->target.controllerVerification.result.outcome
              == ControllerProbeOutcome::NotRun);
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
        CHECK(snapshot->applicationRecovery.state
              == ApplicationRecoveryDiscoveryState::Empty);
        CHECK(snapshot->projectActive);
        CHECK(snapshot->ownership
              == GuidedTunerOwnershipState::RecoveryRequired);
        CHECK(snapshot->recoveryJournalPath == files.RecoveryPath());
        CHECK(fixture.factory->CreateCount() == 0U);
        CHECK(fixture.factory->Projects().empty());
    }

    SUBCASE("application and project-adjacent evidence remain additive")
    {
        TemporaryDirectory home;
        const auto storage = ApplicationStoragePathsForHome(home.Get());
        REQUIRE(EnsureApplicationStorageDirectories(storage).success);
        const auto applicationJournal =
            storage.recoveryDirectory / "pending.recovery";
        REQUIRE(SaveTunerRecoveryJournal(
                    ApplicationRecoveryRecord(
                        ControllerEndpointRequest{
                            ControllerEndpointKind::SshBridge,
                            "controller-remote",
                            41000U,
                            "bridge-alias",
                            "fidget_bridge",
                        }),
                    applicationJournal.string()).success);

        TemporaryLegacyProject files(home.Get());
        WriteText(files.RecoveryPath(), "malformed recovery evidence\n");
        auto transportOwner =
            std::make_unique<test::FakeCommandTransport>();
        auto* transport = transportOwner.get();
        auto factoryOwner = std::make_unique<test::FakeTransportFactory>(
            std::move(transportOwner));
        auto* factory = factoryOwner.get();
        OwnershipService service(std::move(factoryOwner), 1h, storage);

        service.Submit(UseCrateProjectCommand{
            files.ProjectPath(), LegacyProject(), 0U});
        REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
            return snapshot.projectActive
                && snapshot.recoveryJournalStatus
                    == RecoveryJournalStatus::Malformed;
        }));

        const auto snapshot = service.CurrentSnapshot();
        CHECK(snapshot->applicationRecovery.state
              == ApplicationRecoveryDiscoveryState::PendingV5);
        CHECK(snapshot->recoveryJournalStatus
              == RecoveryJournalStatus::Malformed);
        CHECK(snapshot->recoveryJournalPath == files.RecoveryPath());
        CHECK(MakeGuidedTunerInputs(*snapshot).recoveryRecordAvailable);
        CHECK(snapshot->ownership
              == GuidedTunerOwnershipState::RecoveryRequired);
        CHECK(factory->CreateCount() == 0U);
        CHECK(transport->SentRequests().empty());
    }
}
