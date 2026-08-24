#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/ApplicationRecoveryDiscovery.h"
#include "core/ScpRegistry.h"
#include "core/VmeProtocol.h"
#include "fake_transport_factory.h"
#include "hardware/PrepareTuningSessionOperation.h"
#include "hardware/TargetProbeOperation.h"
#include "hardware/VmeTransaction.h"
#include "vme_test_support.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <ostream>
#include <sstream>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {

constexpr std::uint32_t TargetBase = 0x11000000U;
constexpr std::uint16_t CaptureSuperReference = 0x1800U;
constexpr std::uint32_t CaptureStackReference = 0x9C100001U;
constexpr std::uint16_t FirstDaqReference = 0x5000U;

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
                / ("fidget-prepare-session-" + std::to_string(unique)
                   + '-' + std::to_string(index));
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

fidget::TargetModuleAddress TargetAddress()
{
    const auto parsed = fidget::ParseTargetModuleAddress("0x1100");
    REQUIRE(parsed.address.has_value());
    return *parsed.address;
}

std::vector<std::byte> LocalReply(
    const std::uint16_t reference,
    const std::uint16_t* addresses,
    const std::uint32_t* values,
    const std::size_t count)
{
    using namespace fidget;
    using namespace fidget::test;
    std::vector<std::uint32_t> frame{
        (static_cast<std::uint32_t>(MvlcSuperFrameType) << 24U)
            | static_cast<std::uint32_t>(1U + count * 2U),
        MvlcReferenceWordCommand | reference,
    };
    for (std::size_t index = 0U; index < count; ++index)
    {
        frame.push_back(MvlcReadLocalCommand | addresses[index]);
        frame.push_back(values[index]);
    }
    return MakeCommandPacket({frame});
}

void QueueLocalRead(
    fidget::test::FakeCommandTransport& transport,
    const std::uint16_t reference,
    const std::uint16_t* addresses,
    const std::uint32_t* values,
    const std::size_t count)
{
    using namespace fidget;
    using namespace fidget::test;
    const auto request = BuildMvlcLocalRegisterBatchReadRequest(
        reference, addresses, count);
    REQUIRE(request.success);
    transport.QueueExchange({
        EncodeWords(request.words),
        {FakeReceiveAction::Datagram(
            LocalReply(reference, addresses, values, count))},
    });
}

struct PreflightValues
{
    std::uint32_t mvlcHardwareId =
        fidget::TargetProbeExpectedMvlcHardwareId;
    std::uint32_t mvlcFirmware = fidget::TargetProbeExpectedMvlcFirmware;
    std::uint32_t daqMode = 0U;
    std::uint16_t targetHardwareId = fidget::Mdpp32HardwareId;
    std::uint16_t targetFirmware =
        fidget::Mdpp32ScpFirmwareRevisionFw2051;
    std::uint16_t targetAcquisition = 0U;
};

void QueuePreflight(
    fidget::test::FakeCommandTransport& transport,
    const PreflightValues& values = {})
{
    using namespace fidget;
    using namespace fidget::test;
    const std::array<std::uint32_t, 3U> localValues{{
        values.mvlcHardwareId,
        values.mvlcFirmware,
        values.daqMode,
    }};
    QueueLocalRead(
        transport,
        1U,
        TargetProbeMvlcRegisterOrder.data(),
        localValues.data(),
        localValues.size());

    TransactionReferences references{2U, 1U};
    QueueRead(
        transport,
        references,
        TargetBase + TargetProbeMdppRegisterOrder[0U],
        values.targetHardwareId);
    QueueRead(
        transport,
        references,
        TargetBase + TargetProbeMdppRegisterOrder[1U],
        values.targetFirmware);
    QueueRead(
        transport,
        references,
        TargetBase + TargetProbeMdppRegisterOrder[2U],
        values.targetAcquisition);
}

fidget::Fw2051ScpQuadConfiguration QuadValues(const std::uint16_t quad)
{
    fidget::Fw2051ScpQuadConfiguration values;
    values.quad = quad;
    values.timingFilter = static_cast<std::uint16_t>(20U + quad);
    for (std::uint16_t channel = 0U; channel < 4U; ++channel)
    {
        values.poleZero[channel] = static_cast<std::uint16_t>(
            100U + quad * 4U + channel);
        values.thresholds[channel] = static_cast<std::uint16_t>(
            40U + quad * 4U + channel);
    }
    values.gain = static_cast<std::uint16_t>(1000U + quad);
    values.shapingTime = static_cast<std::uint16_t>(160U + quad);
    values.baselineRestorer = static_cast<std::uint16_t>(quad % 4U);
    values.resetTime = static_cast<std::uint16_t>(64U + quad);
    values.signalRiseTime = static_cast<std::uint16_t>(24U + quad);
    values.preSamples = static_cast<std::uint16_t>(32U + quad);
    values.totalSamples = static_cast<std::uint16_t>(200U + quad * 2U);
    values.sampleConfiguration = static_cast<std::uint16_t>(quad % 4U);
    return values;
}

void QueueDaqGate(
    fidget::test::FakeCommandTransport& transport,
    const std::uint16_t reference,
    const std::uint32_t value)
{
    constexpr std::array<std::uint16_t, 1U> address{{0x1300U}};
    const std::array<std::uint32_t, 1U> values{{value}};
    QueueLocalRead(
        transport,
        reference,
        address.data(),
        values.data(),
        values.size());
}

void QueueCapture(
    fidget::test::FakeCommandTransport& transport,
    const std::array<std::uint32_t, 11U>& daqValues = {})
{
    using namespace fidget;
    using namespace fidget::test;
    TransactionReferences references{
        CaptureSuperReference,
        CaptureStackReference,
    };
    std::size_t daqIndex = 0U;
    QueueDaqGate(
        transport,
        static_cast<std::uint16_t>(FirstDaqReference + daqIndex),
        daqValues[daqIndex]);
    ++daqIndex;

    QueueRead(transport, references, TargetBase + 0x6008U, Mdpp32HardwareId);
    QueueRead(
        transport,
        references,
        TargetBase + 0x600EU,
        Mdpp32ScpFirmwareRevisionFw2051);
    QueueRead(transport, references, TargetBase + 0x6010U, 1U);
    QueueRead(transport, references, TargetBase + 0x6044U, 0x18U);

    for (std::uint16_t quad = 0U; quad < Fw2051ScpQuadCount; ++quad)
    {
        QueueDaqGate(
            transport,
            static_cast<std::uint16_t>(FirstDaqReference + daqIndex),
            daqValues[daqIndex]);
        ++daqIndex;
        QueueWrite(
            transport,
            references,
            TargetBase + Fw2051ScpSelectorRegister,
            quad);
        const auto values = QuadValues(quad);
        for (const auto& setting : Fw2051ScpSettingRegistry)
        {
            const auto value = Fw2051ScpQuadRegisterValue(
                values, setting.registerOffset);
            REQUIRE(value.has_value());
            QueueRead(
                transport,
                references,
                TargetBase + setting.registerOffset,
                *value);
        }
    }

    QueueDaqGate(
        transport,
        static_cast<std::uint16_t>(FirstDaqReference + daqIndex),
        daqValues[daqIndex]);
    ++daqIndex;
    QueueWrite(
        transport,
        references,
        TargetBase + Fw2051ScpSelectorRegister,
        0U);
    QueueDaqGate(
        transport,
        static_cast<std::uint16_t>(FirstDaqReference + daqIndex),
        daqValues[daqIndex]);
    ++daqIndex;
    REQUIRE(daqIndex == daqValues.size());
}

struct Fixture
{
    Fixture(
        fidget::TransportEndpointRequest requestedEndpoint =
            fidget::DirectEthernetEndpointRequest{"mvlc-test", 32768U},
        const std::array<std::uint32_t, 11U>& daqValues = {})
        : home()
        , storage(fidget::ApplicationStoragePathsForHome(home.Get()))
        , journal(storage.recoveryDirectory / "session.recovery")
        , endpoint(std::move(requestedEndpoint))
        , transportOwner(
              std::make_unique<fidget::test::FakeCommandTransport>())
        , transport(transportOwner.get())
        , factory(std::move(transportOwner))
    {
        QueuePreflight(*transport);
        QueueCapture(*transport, daqValues);
    }

    [[nodiscard]] fidget::PrepareTuningSessionRequest Request() const
    {
        return {endpoint, TargetAddress(), storage, journal};
    }

    TemporaryDirectory home;
    fidget::ApplicationStoragePaths storage;
    std::filesystem::path journal;
    fidget::TransportEndpointRequest endpoint;
    std::unique_ptr<fidget::test::FakeCommandTransport> transportOwner;
    fidget::test::FakeCommandTransport* transport = nullptr;
    fidget::test::FakeTransportFactory factory;
    std::atomic<bool> cancelled{false};
};

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    std::ostringstream output;
    output << input.rdbuf();
    REQUIRE_FALSE(input.bad());
    return output.str();
}

fidget::TunerRecoveryRecord LoadRecord(const std::filesystem::path& path)
{
    const auto loaded = fidget::LoadTunerRecoveryJournal(path.string());
    INFO(loaded.message);
    REQUIRE(loaded.success);
    REQUIRE(loaded.record.has_value());
    REQUIRE(loaded.record->version5.has_value());
    return *loaded.record;
}

bool IsSnapshotCaptured(const fidget::TunerRecoveryRecord& record)
{
    return record.version5->liveRestoreSnapshot.has_value()
        && !record.version5->selectorParkingRequired;
}

bool IsImmediateStackPlumbingAddress(const std::uint16_t address)
{
    using namespace fidget;
    return (address >= MvlcStackMemoryBegin
            && address < MvlcStackMemoryBegin + 0x0100U)
        || address == MvlcStackExecutionStatus0
        || address == MvlcStackExecutionStatus1
        || address == MvlcStack0OffsetRegister
        || address == MvlcStack0TriggerRegister;
}

fidget::ControllerEndpointRequest RecoveryEndpoint(
    const fidget::TransportEndpointRequest& endpoint)
{
    using namespace fidget;
    if (const auto* direct =
            std::get_if<DirectEthernetEndpointRequest>(&endpoint))
    {
        return {
            ControllerEndpointKind::DirectEthernet,
            direct->mvlcHost,
            direct->mvlcCommandPort,
            {},
            {},
        };
    }
    const auto& bridge = std::get<SshBridgeEndpointRequest>(endpoint);
    return {
        ControllerEndpointKind::SshBridge,
        bridge.mvlcHost,
        bridge.mvlcCommandPort,
        bridge.sshDestination,
        bridge.remoteBridgeCommand,
    };
}

void CheckPreparedIdentity(
    const fidget::TunerRecoveryRecord& record,
    const fidget::TransportEndpointRequest& endpoint)
{
    using namespace fidget;
    CHECK(record.formatVersion == TunerRecoveryJournalV5FormatVersion);
    REQUIRE(record.version5.has_value());
    const auto& data = *record.version5;
    CHECK(data.sessionPhase == TuningSessionPhase::Preparing);
    CHECK(data.endpoint == RecoveryEndpoint(endpoint));
    CHECK(data.identity.mvlcHardwareId == TargetProbeExpectedMvlcHardwareId);
    CHECK(data.identity.mvlcFirmwareRevision == TargetProbeExpectedMvlcFirmware);
    CHECK(data.identity.targetBaseAddress == TargetBase);
    CHECK(data.identity.targetHardwareId == Mdpp32HardwareId);
    CHECK(data.identity.targetFirmwareRevision ==
          Mdpp32ScpFirmwareRevisionFw2051);
    CHECK_FALSE(data.ownership.has_value());
    CHECK(data.deviations.empty());
}

void CheckOnlySelectorWrites(
    const fidget::test::FakeCommandTransport& transport,
    const std::size_t expectedSelectorWrites)
{
    using namespace fidget;
    const auto operations = fidget::test::DecodeWireOperations(transport);
    std::size_t writes = 0U;
    for (const auto& operation : operations)
    {
        if (!operation.write)
            continue;
        ++writes;
        CHECK(operation.address == TargetBase + Fw2051ScpSelectorRegister);
    }
    CHECK(writes == expectedSelectorWrites);
}

} // namespace

TEST_CASE("session preparation captures and promotes the complete snapshot over both endpoints")
{
    using namespace fidget;
    using namespace fidget::test;

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

    Fixture fixture(endpoint);
    bool preparedBeforeFirstSelector = false;
    fixture.transport->SetSendHook([&](const std::vector<std::byte>& request) {
        const auto words = DecodeWords(request);
        if (std::find(
                words.begin(), words.end(), MvlcVmeWriteA32D16Command)
            == words.end())
        {
            return;
        }
        if (preparedBeforeFirstSelector)
            return;
        const auto loaded = LoadTunerRecoveryJournal(fixture.journal.string());
        preparedBeforeFirstSelector = loaded.success
            && loaded.record.has_value()
            && loaded.record->version5.has_value()
            && loaded.record->version5->selectorParkingRequired
            && !loaded.record->version5->liveRestoreSnapshot.has_value();
    });

    std::vector<std::pair<std::chrono::microseconds, std::size_t>> delays;
    PrepareTuningSessionRuntime runtime;
    runtime.delay = [&](const std::chrono::microseconds duration) {
        delays.emplace_back(
            duration, fixture.transport->SentRequests().size());
    };
    const auto result = PrepareTuningSession(
        fixture.factory, fixture.Request(), fixture.cancelled, runtime);

    INFO(result.message);
    CHECK(result.outcome == PrepareTuningSessionOutcome::SnapshotCaptured);
    CHECK(result.journalPhase ==
          PrepareTuningSessionJournalPhase::SnapshotCaptured);
    CHECK(result.preflight.outcome == TargetProbeOutcome::VerifiedIdle);
    CHECK(result.journalCreatedExclusively);
    CHECK(result.journalPromotedAtomically);
    CHECK(result.temporaryConnectionOpened);
    CHECK(result.temporaryConnectionClosed);
    CHECK(result.daqIdleChecks == 11U);
    CHECK(preparedBeforeFirstSelector);
    REQUIRE(delays.size() == 9U);
    for (std::size_t quad = 0U; quad < 8U; ++quad)
    {
        CHECK(delays[quad].first == std::chrono::microseconds(50));
        CHECK(delays[quad].second == 19U + quad * 37U);
    }
    CHECK(delays.back().first == std::chrono::microseconds(50));
    CHECK(delays.back().second == 315U);

    const auto record = LoadRecord(fixture.journal);
    CheckPreparedIdentity(record, endpoint);
    REQUIRE(IsSnapshotCaptured(record));
    const auto& snapshot = *record.version5->liveRestoreSnapshot;
    CHECK(snapshot.state == ScpConfigurationState::Complete);
    CHECK(snapshot.selectorParkedAtQuadZero);
    CHECK(snapshot.quads.size() == Fw2051ScpQuadCount);
    CHECK(Fw2051ScpConfigurationValueCount == 141U);
    CheckOnlySelectorWrites(*fixture.transport, 9U);

    const auto operations = DecodeWireOperations(*fixture.transport);
    REQUIRE(operations.size() == 152U);
    const std::array<std::uint16_t, 7U> firstOffsets{{
        0x6008U, 0x600EU, 0x603AU,
        0x6008U, 0x600EU, 0x6010U, 0x6044U,
    }};
    for (std::size_t index = 0U; index < firstOffsets.size(); ++index)
    {
        CHECK_FALSE(operations[index].write);
        CHECK(operations[index].address == TargetBase + firstOffsets[index]);
    }
    std::size_t daqReadCount = 0U;
    for (const auto& request : fixture.transport->SentRequests())
    {
        const auto words = DecodeWords(request);
        for (const auto word : words)
        {
            if (word == (MvlcReadLocalCommand | 0x1300U))
                ++daqReadCount;
            if ((word & 0xFFFF0000U) == MvlcWriteLocalCommand)
            {
                const auto address = static_cast<std::uint16_t>(word);
                CHECK(IsImmediateStackPlumbingAddress(address));
                CHECK(address != 0x1300U);
            }
        }
    }
    CHECK(daqReadCount == 12U); // one preflight read plus eleven live gates
    REQUIRE(fixture.factory.Requests().size() == 1U);
    CHECK(fixture.factory.Requests().front().index() == endpoint.index());
    CHECK_FALSE(fixture.transport->IsOpen());
}

TEST_CASE("every post-journal checkpoint leaves one conservative discoverable record")
{
    using namespace fidget;

    Fixture reference;
    std::vector<PrepareTuningSessionCheckpoint> checkpoints;
    std::vector<std::size_t> checkpointRequestCounts;
    PrepareTuningSessionRuntime referenceRuntime;
    referenceRuntime.delay = [](std::chrono::microseconds) {};
    referenceRuntime.checkpoint = [&](const auto& checkpoint) {
        checkpoints.push_back(checkpoint);
        checkpointRequestCounts.push_back(
            reference.transport->SentRequests().size());
        return true;
    };
    const auto complete = PrepareTuningSession(
        reference.factory,
        reference.Request(),
        reference.cancelled,
        referenceRuntime);
    REQUIRE(complete.outcome == PrepareTuningSessionOutcome::SnapshotCaptured);
    REQUIRE(checkpoints.size() == 164U);
    REQUIRE(checkpointRequestCounts.size() == checkpoints.size());
    for (std::size_t index = 0U; index < checkpoints.size(); ++index)
        CHECK(checkpoints[index].ordinal == index);
    const auto countKind = [&](const PrepareTuningSessionCheckpointKind kind) {
        return static_cast<std::size_t>(std::count_if(
            checkpoints.begin(),
            checkpoints.end(),
            [kind](const auto& checkpoint) {
                return checkpoint.kind == kind;
            }));
    };
    CHECK(countKind(PrepareTuningSessionCheckpointKind::JournalPrepared)
          == 1U);
    CHECK(countKind(PrepareTuningSessionCheckpointKind::DaqIdleVerified)
          == 11U);
    CHECK(countKind(PrepareTuningSessionCheckpointKind::GlobalRegisterRead)
          == 4U);
    CHECK(countKind(PrepareTuningSessionCheckpointKind::SelectorSettled)
          == 8U);
    CHECK(countKind(PrepareTuningSessionCheckpointKind::BankedRegisterRead)
          == 136U);
    CHECK(countKind(PrepareTuningSessionCheckpointKind::SelectorParked)
          == 1U);
    CHECK(countKind(PrepareTuningSessionCheckpointKind::CaptureCompleted)
          == 1U);
    CHECK(countKind(
              PrepareTuningSessionCheckpointKind::BeforeJournalPromotion)
          == 1U);
    CHECK(countKind(PrepareTuningSessionCheckpointKind::JournalPromoted)
          == 1U);

    for (std::size_t fault = 0U; fault < checkpoints.size(); ++fault)
    {
        CAPTURE(fault);
        CAPTURE(static_cast<int>(checkpoints[fault].kind));
        Fixture fixture;
        PrepareTuningSessionRuntime runtime;
        runtime.delay = [](std::chrono::microseconds) {};
        runtime.checkpoint = [fault](const auto& checkpoint) {
            return checkpoint.ordinal != fault;
        };
        const auto result = PrepareTuningSession(
            fixture.factory,
            fixture.Request(),
            fixture.cancelled,
            runtime);
        CHECK(result.outcome == PrepareTuningSessionOutcome::Interrupted);
        REQUIRE(result.interruptedAt.has_value());
        CHECK(result.interruptedAt->ordinal == fault);
        CHECK(result.temporaryConnectionClosed);
        CHECK_FALSE(fixture.transport->IsOpen());
        CHECK(fixture.transport->SentRequests().size()
              == checkpointRequestCounts[fault]);

        const auto beforeDiscovery = ReadText(fixture.journal);
        const auto record = LoadRecord(fixture.journal);
        CheckPreparedIdentity(record, fixture.endpoint);
        const bool promoted = checkpoints[fault].kind
            == PrepareTuningSessionCheckpointKind::JournalPromoted;
        CHECK(IsSnapshotCaptured(record) == promoted);
        if (!promoted)
        {
            CHECK(record.version5->selectorParkingRequired);
            CHECK_FALSE(record.version5->liveRestoreSnapshot.has_value());
        }
        else
        {
            CHECK_FALSE(record.version5->selectorParkingRequired);
            REQUIRE(record.version5->liveRestoreSnapshot.has_value());
            CHECK(record.version5->liveRestoreSnapshot->quads.size() == 8U);
        }

        const auto discovery = DiscoverApplicationRecovery(fixture.storage);
        INFO(discovery.message);
        CHECK(discovery.state ==
              ApplicationRecoveryDiscoveryState::PendingV5);
        REQUIRE(discovery.record.has_value());
        CHECK(ReadText(fixture.journal) == beforeDiscovery);
        CHECK(std::distance(
                  std::filesystem::directory_iterator(
                      fixture.storage.recoveryDirectory),
                  std::filesystem::directory_iterator{}) == 1);

        const auto operations =
            fidget::test::DecodeWireOperations(*fixture.transport);
        REQUIRE(operations.size() >= 3U);
        CHECK_FALSE(operations[0U].write);
        CHECK_FALSE(operations[1U].write);
        CHECK_FALSE(operations[2U].write);
        if (promoted)
            CheckOnlySelectorWrites(*fixture.transport, 9U);
    }
}

TEST_CASE("exclusive creation refuses an existing record after read-only revalidation")
{
    using namespace fidget;
    using namespace fidget::test;

    Fixture fixture;
    TunerRecoveryRecord existing;
    existing.formatVersion = TunerRecoveryJournalV5FormatVersion;
    TunerRecoveryV5Data data;
    data.sessionPhase = TuningSessionPhase::Preparing;
    data.endpoint = RecoveryEndpoint(fixture.endpoint);
    data.identity = {
        TargetProbeExpectedMvlcHardwareId,
        TargetProbeExpectedMvlcFirmware,
        TargetBase,
        Mdpp32HardwareId,
        Mdpp32ScpFirmwareRevisionFw2051,
    };
    data.selectorParkingRequired = true;
    existing.version5 = std::move(data);
    REQUIRE(EnsureApplicationStorageDirectories(fixture.storage).success);
    REQUIRE(CreateTunerRecoveryJournalExclusive(
        existing, fixture.journal.string()).success);
    const auto original = ReadText(fixture.journal);

    PrepareTuningSessionRuntime runtime;
    runtime.delay = [](std::chrono::microseconds) {};
    const auto result = PrepareTuningSession(
        fixture.factory, fixture.Request(), fixture.cancelled, runtime);

    CHECK(result.outcome == PrepareTuningSessionOutcome::RecoveryRecordExists);
    CHECK(result.preflight.outcome == TargetProbeOutcome::VerifiedIdle);
    CHECK_FALSE(result.journalCreatedExclusively);
    CHECK(result.journalPhase == PrepareTuningSessionJournalPhase::None);
    CHECK(ReadText(fixture.journal) == original);
    const auto operations = DecodeWireOperations(*fixture.transport);
    REQUIRE(operations.size() == 3U);
    for (const auto& operation : operations)
        CHECK_FALSE(operation.write);
}

TEST_CASE("a failed atomic promotion retains the Prepared bytes exactly")
{
    using namespace fidget;

    Fixture fixture;
    std::string preparedBytes;
    std::size_t writes = 0U;
    PrepareTuningSessionRuntime runtime;
    runtime.delay = [](std::chrono::microseconds) {};
    runtime.journalRuntime.writer = [&](
        std::ostream& output, std::string_view text) {
        ++writes;
        if (writes == 1U)
        {
            output.write(text.data(), static_cast<std::streamsize>(text.size()));
            preparedBytes.assign(text);
            return true;
        }
        const auto partial = text.substr(0U, text.size() / 2U);
        output.write(
            partial.data(), static_cast<std::streamsize>(partial.size()));
        return false;
    };

    const auto result = PrepareTuningSession(
        fixture.factory, fixture.Request(), fixture.cancelled, runtime);
    CHECK(result.outcome ==
          PrepareTuningSessionOutcome::JournalPromotionFailed);
    CHECK(result.journalCreatedExclusively);
    CHECK_FALSE(result.journalPromotedAtomically);
    CHECK(result.journalPhase == PrepareTuningSessionJournalPhase::Prepared);
    CHECK(writes == 2U);
    CHECK(ReadText(fixture.journal) == preparedBytes);
    const auto record = LoadRecord(fixture.journal);
    CHECK(record.version5->selectorParkingRequired);
    CHECK_FALSE(record.version5->liveRestoreSnapshot.has_value());
    CHECK(DiscoverApplicationRecovery(fixture.storage).state ==
          ApplicationRecoveryDiscoveryState::PendingV5);
}

TEST_CASE("a failed exclusive temporary write installs no record or selector state")
{
    using namespace fidget;
    using namespace fidget::test;

    Fixture fixture;
    PrepareTuningSessionRuntime runtime;
    runtime.delay = [](std::chrono::microseconds) {};
    runtime.journalRuntime.writer = [](
        std::ostream& output, std::string_view text) {
        const auto partial = text.substr(0U, text.size() / 2U);
        output.write(
            partial.data(), static_cast<std::streamsize>(partial.size()));
        return false;
    };
    const auto result = PrepareTuningSession(
        fixture.factory, fixture.Request(), fixture.cancelled, runtime);

    CHECK(result.outcome == PrepareTuningSessionOutcome::StorageUnavailable);
    CHECK_FALSE(result.journalCreatedExclusively);
    CHECK(result.journalPhase == PrepareTuningSessionJournalPhase::None);
    CHECK_FALSE(std::filesystem::exists(fixture.journal));
    CHECK(DiscoverApplicationRecovery(fixture.storage).state ==
          ApplicationRecoveryDiscoveryState::Empty);
    const auto operations = DecodeWireOperations(*fixture.transport);
    REQUIRE(operations.size() == 3U);
    for (const auto& operation : operations)
        CHECK_FALSE(operation.write);
}

TEST_CASE("v5 preparation uses the shared same-filesystem staging and sync order")
{
    using namespace fidget;

    Fixture fixture;
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>>
        filesystemChecks;
    std::vector<std::filesystem::path> synchronized;
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>>
        replacements;
    PrepareTuningSessionRuntime runtime;
    runtime.delay = [](std::chrono::microseconds) {};
    runtime.journalRuntime.sameFilesystem =
        [&](const std::string& staging,
            const std::string& authority,
            std::string&) {
            filesystemChecks.emplace_back(staging, authority);
            return true;
        };
    runtime.journalRuntime.synchronize =
        [&](const std::string& path, std::string&) {
            synchronized.emplace_back(path);
            return true;
        };
    runtime.journalRuntime.replace =
        [&](const std::string& source,
            const std::string& destination,
            std::string& error) {
            replacements.emplace_back(source, destination);
            std::error_code renameError;
            std::filesystem::rename(source, destination, renameError);
            if (renameError)
            {
                error = renameError.message();
                return false;
            }
            return true;
        };

    const auto result = PrepareTuningSession(
        fixture.factory, fixture.Request(), fixture.cancelled, runtime);
    INFO(result.message);
    REQUIRE(result.outcome == PrepareTuningSessionOutcome::SnapshotCaptured);
    REQUIRE(filesystemChecks.size() == 2U);
    REQUIRE(synchronized.size() == 4U);
    REQUIRE(replacements.size() == 1U);

    for (const auto& check : filesystemChecks)
    {
        CHECK(check.first.parent_path()
              == fixture.storage.recoveryDirectory);
        CHECK(check.first.filename().string().rfind(
                  std::string(TunerRecoveryJournalStagingDirectoryPrefix),
                  0U)
              == 0U);
    }
    CHECK(filesystemChecks[0U].second
          == fixture.storage.recoveryDirectory);
    CHECK(filesystemChecks[1U].second == fixture.journal);
    CHECK(synchronized[0U].filename() == "journal");
    CHECK(synchronized[0U].parent_path().parent_path()
          == fixture.storage.recoveryDirectory);
    CHECK(synchronized[1U] == fixture.storage.recoveryDirectory);
    CHECK(synchronized[2U].filename() == "journal");
    CHECK(synchronized[2U].parent_path().parent_path()
          == fixture.storage.recoveryDirectory);
    CHECK(synchronized[3U] == fixture.storage.recoveryDirectory);
    CHECK(replacements.front().first == synchronized[2U]);
    CHECK(replacements.front().second == fixture.journal);

    for (const auto& entry :
         std::filesystem::directory_iterator(
             fixture.storage.recoveryDirectory))
    {
        CHECK(entry.path() == fixture.journal);
    }
}

TEST_CASE("v5 preparation reports installed authority after directory sync failure")
{
    using namespace fidget;
    using namespace fidget::test;

    SUBCASE("exclusive Prepared install is retained")
    {
        Fixture fixture;
        std::size_t syncCalls = 0U;
        PrepareTuningSessionRuntime runtime;
        runtime.delay = [](std::chrono::microseconds) {};
        runtime.journalRuntime.synchronize =
            [&](const std::string&, std::string& error) {
                ++syncCalls;
                if (syncCalls == 2U)
                {
                    error = "injected recovery-directory sync failure";
                    return false;
                }
                return true;
            };
        const auto result = PrepareTuningSession(
            fixture.factory, fixture.Request(), fixture.cancelled, runtime);
        INFO(result.message);
        CHECK(result.outcome
              == PrepareTuningSessionOutcome::StorageUnavailable);
        CHECK(result.journalCreatedExclusively);
        CHECK(result.journalPhase
              == PrepareTuningSessionJournalPhase::Prepared);
        const auto record = LoadRecord(fixture.journal);
        CHECK(record.version5->selectorParkingRequired);
        CHECK_FALSE(record.version5->liveRestoreSnapshot.has_value());
        CHECK(DiscoverApplicationRecovery(fixture.storage).state
              == ApplicationRecoveryDiscoveryState::PendingV5);
        const auto operations = DecodeWireOperations(*fixture.transport);
        REQUIRE(operations.size() == 3U);
        for (const auto& operation : operations)
            CHECK_FALSE(operation.write);
    }

    SUBCASE("promoted Snapshot-captured install is retained")
    {
        Fixture fixture;
        std::size_t syncCalls = 0U;
        PrepareTuningSessionRuntime runtime;
        runtime.delay = [](std::chrono::microseconds) {};
        runtime.journalRuntime.synchronize =
            [&](const std::string&, std::string& error) {
                ++syncCalls;
                if (syncCalls == 4U)
                {
                    error = "injected recovery-directory sync failure";
                    return false;
                }
                return true;
            };
        const auto result = PrepareTuningSession(
            fixture.factory, fixture.Request(), fixture.cancelled, runtime);
        INFO(result.message);
        CHECK(result.outcome
              == PrepareTuningSessionOutcome::JournalPromotionFailed);
        CHECK(result.journalCreatedExclusively);
        CHECK(result.journalPromotedAtomically);
        CHECK(result.journalPhase
              == PrepareTuningSessionJournalPhase::SnapshotCaptured);
        const auto record = LoadRecord(fixture.journal);
        CHECK(IsSnapshotCaptured(record));
        CHECK(DiscoverApplicationRecovery(fixture.storage).state
              == ApplicationRecoveryDiscoveryState::PendingV5);
        CheckOnlySelectorWrites(*fixture.transport, 9U);
    }
}

TEST_CASE("v5 preparation refuses unproven same-filesystem staging before selectors")
{
    using namespace fidget;
    using namespace fidget::test;

    Fixture fixture;
    PrepareTuningSessionRuntime runtime;
    runtime.delay = [](std::chrono::microseconds) {};
    runtime.journalRuntime.sameFilesystem =
        [](const std::string&, const std::string&, std::string& error) {
            error = "injected cross-device staging refusal";
            return false;
        };
    const auto result = PrepareTuningSession(
        fixture.factory, fixture.Request(), fixture.cancelled, runtime);
    INFO(result.message);
    CHECK(result.outcome == PrepareTuningSessionOutcome::StorageUnavailable);
    CHECK_FALSE(result.journalCreatedExclusively);
    CHECK_FALSE(std::filesystem::exists(fixture.journal));
    CHECK(DiscoverApplicationRecovery(fixture.storage).state
          == ApplicationRecoveryDiscoveryState::Empty);
    const auto operations = DecodeWireOperations(*fixture.transport);
    REQUIRE(operations.size() == 3U);
    for (const auto& operation : operations)
        CHECK_FALSE(operation.write);
}

TEST_CASE("every identity or stopped-state rejection precedes journal and VME writes")
{
    using namespace fidget;
    using namespace fidget::test;

    struct Rejection
    {
        TargetProbeOutcome expected;
        PreflightValues values;
    };
    const std::array<Rejection, 6U> rejections{{
        {TargetProbeOutcome::WrongMvlcIdentity,
         PreflightValues{0x1234U, TargetProbeExpectedMvlcFirmware}},
        {TargetProbeOutcome::WrongMvlcFirmware,
         PreflightValues{
             TargetProbeExpectedMvlcHardwareId, 0x0045U}},
        {TargetProbeOutcome::ControllerDaqActive,
         PreflightValues{
             TargetProbeExpectedMvlcHardwareId,
             TargetProbeExpectedMvlcFirmware,
             1U}},
        {TargetProbeOutcome::WrongTargetIdentity,
         PreflightValues{
             TargetProbeExpectedMvlcHardwareId,
             TargetProbeExpectedMvlcFirmware,
             0U,
             0x1234U}},
        {TargetProbeOutcome::WrongTargetFirmware,
         PreflightValues{
             TargetProbeExpectedMvlcHardwareId,
             TargetProbeExpectedMvlcFirmware,
             0U,
             Mdpp32HardwareId,
             0x2050U}},
        {TargetProbeOutcome::TargetAcquisitionActive,
         PreflightValues{
             TargetProbeExpectedMvlcHardwareId,
             TargetProbeExpectedMvlcFirmware,
             0U,
             Mdpp32HardwareId,
             Mdpp32ScpFirmwareRevisionFw2051,
             1U}},
    }};

    for (const auto& rejection : rejections)
    {
        CAPTURE(static_cast<int>(rejection.expected));
        TemporaryDirectory home;
        const auto storage = ApplicationStoragePathsForHome(home.Get());
        const auto journal = storage.recoveryDirectory / "session.recovery";
        auto owner = std::make_unique<FakeCommandTransport>();
        auto* transport = owner.get();
        QueuePreflight(*transport, rejection.values);
        FakeTransportFactory factory(std::move(owner));
        std::atomic<bool> cancelled{false};
        const auto result = PrepareTuningSession(
            factory,
            {
                DirectEthernetEndpointRequest{"mvlc-test", 32768U},
                TargetAddress(),
                storage,
                journal,
            },
            cancelled,
            {});
        CHECK(result.outcome == PrepareTuningSessionOutcome::PreflightRefused);
        CHECK(result.preflight.outcome == rejection.expected);
        CHECK_FALSE(std::filesystem::exists(journal));
        const auto operations = DecodeWireOperations(*transport);
        for (const auto& operation : operations)
            CHECK_FALSE(operation.write);
    }
}

TEST_CASE("read-only recognition of MDPP-32 v2 never authorizes preparation writes")
{
    using namespace fidget;
    using namespace fidget::test;

    TemporaryDirectory home;
    const auto storage = ApplicationStoragePathsForHome(home.Get());
    const auto journal = storage.recoveryDirectory / "session.recovery";
    auto owner = std::make_unique<FakeCommandTransport>();
    auto* transport = owner.get();
    PreflightValues values;
    values.targetHardwareId = Mdpp32AlternateHardwareId;
    QueuePreflight(*transport, values);
    FakeTransportFactory factory(std::move(owner));
    std::atomic<bool> cancelled{false};

    const auto result = PrepareTuningSession(
        factory,
        {
            DirectEthernetEndpointRequest{"mvlc-test", 32768U},
            TargetAddress(),
            storage,
            journal,
        },
        cancelled,
        {});

    INFO(result.message);
    CHECK(result.preflight.outcome == TargetProbeOutcome::VerifiedIdle);
    CHECK(result.preflight.targetHardwareId == Mdpp32AlternateHardwareId);
    CHECK(result.outcome == PrepareTuningSessionOutcome::PreflightRefused);
    CHECK(result.message.find("0x500C") != std::string::npos);
    CHECK(result.message.find("recorded hardware acceptance")
          != std::string::npos);
    CHECK_FALSE(std::filesystem::exists(journal));
    CHECK_FALSE(std::filesystem::exists(storage.recoveryDirectory));
    const auto operations = DecodeWireOperations(*transport);
    REQUIRE(operations.size() == 3U);
    for (const auto& operation : operations)
        CHECK_FALSE(operation.write);
    for (const auto& request : transport->SentRequests())
    {
        const auto words = DecodeWords(request);
        CHECK(std::find(
                  words.begin(),
                  words.end(),
                  MvlcVmeWriteA32D16Command)
              == words.end());
    }
}

TEST_CASE("activity detected before or during preparation never permits a blind write")
{
    using namespace fidget;
    using namespace fidget::test;

    SUBCASE("controller active during initial zero-write revalidation")
    {
        TemporaryDirectory home;
        const auto storage = ApplicationStoragePathsForHome(home.Get());
        const auto journal = storage.recoveryDirectory / "session.recovery";
        auto owner = std::make_unique<FakeCommandTransport>();
        auto* transport = owner.get();
        PreflightValues values;
        values.daqMode = 1U;
        QueuePreflight(*transport, values);
        FakeTransportFactory factory(std::move(owner));
        std::atomic<bool> cancelled{false};
        const auto result = PrepareTuningSession(
            factory,
            {
                DirectEthernetEndpointRequest{"mvlc-test", 32768U},
                TargetAddress(),
                storage,
                journal,
            },
            cancelled,
            {});
        CHECK(result.outcome == PrepareTuningSessionOutcome::PreflightRefused);
        CHECK(result.preflight.outcome == TargetProbeOutcome::ControllerDaqActive);
        CHECK_FALSE(std::filesystem::exists(journal));
        CHECK(DecodeWireOperations(*transport).empty());
    }

    SUBCASE("DAQ takeover after quad zero stops before the next selector")
    {
        std::array<std::uint32_t, 11U> daqValues{};
        daqValues[2U] = 1U;
        Fixture fixture(
            DirectEthernetEndpointRequest{"mvlc-test", 32768U},
            daqValues);
        PrepareTuningSessionRuntime runtime;
        runtime.delay = [](std::chrono::microseconds) {};
        const auto result = PrepareTuningSession(
            fixture.factory, fixture.Request(), fixture.cancelled, runtime);
        CHECK(result.outcome ==
              PrepareTuningSessionOutcome::ControllerBecameActive);
        CHECK(result.controllerBecameActive);
        const auto record = LoadRecord(fixture.journal);
        CHECK(record.version5->selectorParkingRequired);
        CHECK_FALSE(record.version5->liveRestoreSnapshot.has_value());
        CheckOnlySelectorWrites(*fixture.transport, 1U);
    }
}
