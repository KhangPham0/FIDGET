#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/ApplicationRecoveryDiscovery.h"
#include "core/ScpConfiguration.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

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
                / ("fidget-recovery-discovery-"
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

fidget::ControllerEndpointRequest DirectEndpoint(
    std::string host = "controller-test",
    const std::uint16_t port = 32768U)
{
    return {
        fidget::ControllerEndpointKind::DirectEthernet,
        std::move(host),
        port,
        {},
        {},
    };
}

fidget::ControllerEndpointRequest BridgeEndpoint(
    std::string host,
    const std::uint16_t port,
    std::string destination,
    std::string command)
{
    return {
        fidget::ControllerEndpointKind::SshBridge,
        std::move(host),
        port,
        std::move(destination),
        std::move(command),
    };
}

fidget::TunerRecoveryRecord MakeV5Record(
    fidget::ControllerEndpointRequest endpoint = DirectEndpoint())
{
    using namespace fidget;

    TunerRecoveryRecord record;
    record.formatVersion = TunerRecoveryJournalV5FormatVersion;
    TunerRecoveryV5Data data;
    data.sessionPhase = TuningSessionPhase::Preparing;
    data.endpoint = std::move(endpoint);
    data.identity.mvlcHardwareId = 0x5008U;
    data.identity.mvlcFirmwareRevision = 0x0046U;
    data.identity.targetBaseAddress = 0x11000000U;
    data.identity.targetHardwareId = Mdpp32HardwareId;
    data.identity.targetFirmwareRevision =
        Mdpp32ScpFirmwareRevisionFw2051;
    data.selectorParkingRequired = true;
    record.version5 = std::move(data);
    return record;
}

fidget::ApplicationStoragePaths ReadyStorage(
    const std::filesystem::path& home)
{
    const auto paths = fidget::ApplicationStoragePathsForHome(home);
    const auto ready = fidget::EnsureApplicationStorageDirectories(paths);
    INFO(ready.message);
    REQUIRE(ready.success);
    return paths;
}

void WriteText(
    const std::filesystem::path& path,
    const std::string& text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    REQUIRE(output.good());
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    std::ostringstream output;
    output << input.rdbuf();
    REQUIRE_FALSE(input.bad());
    return output.str();
}

std::string SerializedV5()
{
    const auto serialized = fidget::SerializeTunerRecoveryJournal(
        MakeV5Record());
    INFO(serialized.message);
    REQUIRE(serialized.success);
    return serialized.text;
}

constexpr const char* ValidV1Journal =
    "MWW_TUNER_RECOVERY 1\n"
    "ENDPOINT mvlc-test 32768\n"
    "MVLC 20488 70\n"
    "MDPP 285212672 20487 1 24\n"
    "STACK 4356 64 4612 512 8732 305441741\n"
    "STATE 2 1 7 24858 200 510\n"
    "CHECKSUM 4158629254257455462\n"
    "END\n";

constexpr const char* ValidV2Journal =
    "MWW_TUNER_RECOVERY 2\n"
    "ENDPOINT mvlc-test 32768\n"
    "MVLC 20488 70\n"
    "MDPP 285212672 20487 1 24\n"
    "ISOLATED 2 858980352 1145307136\n"
    "STACK 4356 64 4612 512 8732 305441741\n"
    "STATE 2 1 7 24858 200 510\n"
    "CHECKSUM 1388001408713401195\n"
    "END\n";

constexpr const char* ValidV3Journal =
    "MWW_TUNER_RECOVERY 3\n"
    "ENDPOINT mvlc-test 32768\n"
    "MVLC 20488 70\n"
    "MDPP 285212672 20487 1 24\n"
    "ISOLATED 2 858980352 1145307136\n"
    "STACK 4356 64 4612 512 8732 305441741\n"
    "SOURCE 1 7 64\n"
    "STATE 2 1 7 24858 200 510\n"
    "CHECKSUM 16597571028148822462\n"
    "END\n";

constexpr const char* ValidV4Journal =
    "MWW_TUNER_RECOVERY 4\n"
    "ENDPOINT mvlc-test 32768\n"
    "MVLC 20488 70\n"
    "MDPP 285212672 20487 1 24\n"
    "ISOLATED 2 858980352 1145307136\n"
    "STACK 4356 64 4612 512 8732 305441741\n"
    "SOURCE 1 7 64 67\n"
    "STATE 2 1 7 24858 200 510\n"
    "CHECKSUM 15109459991863664248\n"
    "END\n";

} // namespace

TEST_CASE("application recovery discovery treats missing and empty directories as clear")
{
    using namespace fidget;

    TemporaryDirectory temporary;
    const auto paths = ApplicationStoragePathsForHome(temporary.Get());
    REQUIRE_FALSE(std::filesystem::exists(paths.stateDirectory));

    const auto missing = DiscoverApplicationRecovery(paths);
    CHECK(missing.state == ApplicationRecoveryDiscoveryState::Empty);
    CHECK(missing.blockReason == ApplicationRecoveryBlockReason::None);
    CHECK(missing.recordPaths.empty());
    CHECK_FALSE(missing.record.has_value());
    CHECK_FALSE(ApplicationRecoveryBlocksNormalTuning(missing));
    CHECK_FALSE(ApplicationRecoveryHasRetainedEvidence(missing));
    CHECK_FALSE(std::filesystem::exists(paths.stateDirectory));

    const auto readyPaths = ReadyStorage(temporary.Get());
    const auto empty = DiscoverApplicationRecovery(readyPaths);
    CHECK(empty.state == ApplicationRecoveryDiscoveryState::Empty);
    CHECK(empty.recordPaths.empty());
    CHECK_FALSE(ApplicationRecoveryBlocksNormalTuning(empty));
}

TEST_CASE("CX-12F staging entries are never recovery evidence")
{
    using namespace fidget;

    TemporaryDirectory temporary;
    const auto paths = ReadyStorage(temporary.Get());
    const auto staging = paths.recoveryDirectory
        / std::string(TunerRecoveryJournalStagingDirectoryPrefix);
    const auto numberedStaging = paths.recoveryDirectory
        / (std::string(TunerRecoveryJournalStagingDirectoryPrefix) + ".7");
    REQUIRE(std::filesystem::create_directory(staging));
    REQUIRE(std::filesystem::create_directory(numberedStaging));
    WriteText(staging / "journal", "abandoned partial journal\n");
    WriteText(numberedStaging / "journal", SerializedV5());
    const auto partialBefore = ReadText(staging / "journal");
    const auto completeBefore = ReadText(numberedStaging / "journal");

    const auto onlyStaging = DiscoverApplicationRecovery(paths);
    CHECK(onlyStaging.state == ApplicationRecoveryDiscoveryState::Empty);
    CHECK(onlyStaging.recordPaths.empty());
    CHECK_FALSE(ApplicationRecoveryBlocksNormalTuning(onlyStaging));
    CHECK_FALSE(ApplicationRecoveryHasRetainedEvidence(onlyStaging));

    const auto journal = paths.recoveryDirectory / "pending.recovery";
    const auto saved = SaveTunerRecoveryJournal(
        MakeV5Record(), journal.string());
    INFO(saved.message);
    REQUIRE(saved.success);
    const auto withRecord = DiscoverApplicationRecovery(paths);
    CHECK(withRecord.state == ApplicationRecoveryDiscoveryState::PendingV5);
    REQUIRE(withRecord.recordPaths.size() == 1U);
    CHECK(withRecord.recordPaths.front() == journal);
    CHECK(ReadText(staging / "journal") == partialBefore);
    CHECK(ReadText(numberedStaging / "journal") == completeBefore);
}

TEST_CASE("one valid v5 record preserves direct and SSH endpoint evidence")
{
    using namespace fidget;

    SUBCASE("direct Ethernet")
    {
        TemporaryDirectory temporary;
        const auto paths = ReadyStorage(temporary.Get());
        const auto journal = paths.recoveryDirectory / "pending.recovery";
        const auto saved = SaveTunerRecoveryJournal(
            MakeV5Record(DirectEndpoint("controller-direct", 32768U)),
            journal.string());
        INFO(saved.message);
        REQUIRE(saved.success);
        const auto bytesBefore = ReadText(journal);

        const auto discovery = DiscoverApplicationRecovery(paths);
        CHECK(discovery.state
              == ApplicationRecoveryDiscoveryState::PendingV5);
        CHECK(discovery.blockReason == ApplicationRecoveryBlockReason::None);
        CHECK(ApplicationRecoveryBlocksNormalTuning(discovery));
        CHECK(ApplicationRecoveryHasRetainedEvidence(discovery));
        REQUIRE(discovery.record.has_value());
        REQUIRE(discovery.record->version5.has_value());
        const auto& data = *discovery.record->version5;
        const auto& endpoint = data.endpoint;
        CHECK(endpoint.kind == ControllerEndpointKind::DirectEthernet);
        CHECK(endpoint.mvlcHost == "controller-direct");
        CHECK(endpoint.mvlcCommandPort == 32768U);
        CHECK(data.identity.targetBaseAddress == 0x11000000U);
        CHECK(data.identity.targetFirmwareRevision
              == Mdpp32ScpFirmwareRevisionFw2051);
        CHECK(ReadText(journal) == bytesBefore);
    }

    SUBCASE("SSH bridge")
    {
        TemporaryDirectory temporary;
        const auto paths = ReadyStorage(temporary.Get());
        const auto journal = paths.recoveryDirectory / "pending.recovery";
        const auto saved = SaveTunerRecoveryJournal(
            MakeV5Record(BridgeEndpoint(
                "controller-remote",
                41000U,
                "bridge-alias",
                "fidget_bridge")),
            journal.string());
        INFO(saved.message);
        REQUIRE(saved.success);
        const auto bytesBefore = ReadText(journal);

        const auto discovery = DiscoverApplicationRecovery(paths);
        REQUIRE(discovery.state
                == ApplicationRecoveryDiscoveryState::PendingV5);
        REQUIRE(discovery.record.has_value());
        REQUIRE(discovery.record->version5.has_value());
        const auto& endpoint = discovery.record->version5->endpoint;
        CHECK(endpoint.kind == ControllerEndpointKind::SshBridge);
        CHECK(endpoint.mvlcHost == "controller-remote");
        CHECK(endpoint.mvlcCommandPort == 41000U);
        CHECK(endpoint.sshDestination == "bridge-alias");
        CHECK(endpoint.remoteBridgeCommand == "fidget_bridge");
        CHECK(ReadText(journal) == bytesBefore);
    }
}

TEST_CASE("multiple application recovery entries block and remain byte-identical")
{
    using namespace fidget;

    TemporaryDirectory temporary;
    const auto paths = ReadyStorage(temporary.Get());
    const auto first = paths.recoveryDirectory / "first.recovery";
    const auto second = paths.recoveryDirectory / "second.recovery";
    REQUIRE(SaveTunerRecoveryJournal(
                MakeV5Record(), first.string()).success);
    REQUIRE(SaveTunerRecoveryJournal(
                MakeV5Record(), second.string()).success);
    const auto firstBefore = ReadText(first);
    const auto secondBefore = ReadText(second);

    const auto discovery = DiscoverApplicationRecovery(paths);
    CHECK(discovery.state == ApplicationRecoveryDiscoveryState::Blocked);
    CHECK(discovery.blockReason
          == ApplicationRecoveryBlockReason::MultipleRecords);
    CHECK(discovery.recordPaths.size() == 2U);
    CHECK(ApplicationRecoveryHasRetainedEvidence(discovery));
    CHECK_FALSE(discovery.record.has_value());
    CHECK(discovery.message.find("multiple entries") != std::string::npos);
    CHECK(ReadText(first) == firstBefore);
    CHECK(ReadText(second) == secondBefore);
}

TEST_CASE("each malformed application recovery class blocks without repair")
{
    using namespace fidget;

    SUBCASE("truncated record")
    {
        TemporaryDirectory temporary;
        const auto paths = ReadyStorage(temporary.Get());
        const auto journal = paths.recoveryDirectory / "truncated.recovery";
        auto text = SerializedV5();
        text.resize(text.size() / 2U);
        WriteText(journal, text);

        const auto discovery = DiscoverApplicationRecovery(paths);
        CHECK(discovery.state == ApplicationRecoveryDiscoveryState::Blocked);
        CHECK(discovery.blockReason
              == ApplicationRecoveryBlockReason::MalformedRecord);
        CHECK(discovery.message.find("malformed") != std::string::npos);
        CHECK(ReadText(journal) == text);
    }

    SUBCASE("checksum corruption")
    {
        TemporaryDirectory temporary;
        const auto paths = ReadyStorage(temporary.Get());
        const auto journal = paths.recoveryDirectory / "checksum.recovery";
        auto text = SerializedV5();
        const auto host = text.find("controller-test");
        REQUIRE(host != std::string::npos);
        text[host] = 'k';
        WriteText(journal, text);

        const auto discovery = DiscoverApplicationRecovery(paths);
        CHECK(discovery.state == ApplicationRecoveryDiscoveryState::Blocked);
        CHECK(discovery.blockReason
              == ApplicationRecoveryBlockReason::MalformedRecord);
        CHECK(discovery.message.find("checksum") != std::string::npos);
        CHECK(ReadText(journal) == text);
    }

    SUBCASE("unsupported future version")
    {
        TemporaryDirectory temporary;
        const auto paths = ReadyStorage(temporary.Get());
        const auto journal = paths.recoveryDirectory / "future.recovery";
        auto text = SerializedV5();
        const auto header = text.find("MWW_TUNER_RECOVERY 5");
        REQUIRE(header != std::string::npos);
        text[header + std::string("MWW_TUNER_RECOVERY ").size()] = '6';
        WriteText(journal, text);

        const auto discovery = DiscoverApplicationRecovery(paths);
        CHECK(discovery.state == ApplicationRecoveryDiscoveryState::Blocked);
        CHECK(discovery.blockReason
              == ApplicationRecoveryBlockReason::MalformedRecord);
        CHECK(ReadText(journal) == text);
    }

    SUBCASE("non-file entry")
    {
        TemporaryDirectory temporary;
        const auto paths = ReadyStorage(temporary.Get());
        const auto entry = paths.recoveryDirectory / "entry.recovery";
        REQUIRE(std::filesystem::create_directory(entry));

        const auto discovery = DiscoverApplicationRecovery(paths);
        CHECK(discovery.state == ApplicationRecoveryDiscoveryState::Blocked);
        CHECK(discovery.blockReason
              == ApplicationRecoveryBlockReason::MalformedRecord);
        CHECK(std::filesystem::is_directory(entry));
    }

    SUBCASE("recovery directory path collision")
    {
        TemporaryDirectory temporary;
        const auto paths = ApplicationStoragePathsForHome(temporary.Get());
        REQUIRE(std::filesystem::create_directory(paths.stateDirectory));
        WriteText(paths.recoveryDirectory, "path collision\n");

        const auto discovery = DiscoverApplicationRecovery(paths);
        CHECK(discovery.state == ApplicationRecoveryDiscoveryState::Blocked);
        CHECK(discovery.blockReason
              == ApplicationRecoveryBlockReason::StorageUnavailable);
        CHECK_FALSE(ApplicationRecoveryHasRetainedEvidence(discovery));
        CHECK(ReadText(paths.recoveryDirectory) == "path collision\n");
    }
}

TEST_CASE("every pre-v5 application recovery format blocks and stays unchanged")
{
    using namespace fidget;

    const std::vector<std::pair<unsigned int, const char*>> legacy{{
        {1U, ValidV1Journal},
        {2U, ValidV2Journal},
        {3U, ValidV3Journal},
        {4U, ValidV4Journal},
    }};
    for (const auto& entry : legacy)
    {
        const auto version = entry.first;
        const auto* text = entry.second;
        CAPTURE(version);
        TemporaryDirectory temporary;
        const auto paths = ReadyStorage(temporary.Get());
        const auto journal = paths.recoveryDirectory / "legacy.recovery";
        WriteText(journal, text);
        const auto bytesBefore = ReadText(journal);

        const auto discovery = DiscoverApplicationRecovery(paths);
        CHECK(discovery.state
              == ApplicationRecoveryDiscoveryState::Blocked);
        CHECK(discovery.blockReason
              == ApplicationRecoveryBlockReason::UnsupportedLegacyVersion);
        CHECK(discovery.message.find(
                  "version " + std::to_string(version))
              != std::string::npos);
        CHECK_FALSE(discovery.record.has_value());
        CHECK(ReadText(journal) == bytesBefore);
    }
}

TEST_CASE("mixed valid and malformed recovery entries block without choosing one")
{
    using namespace fidget;

    TemporaryDirectory temporary;
    const auto paths = ReadyStorage(temporary.Get());
    const auto valid = paths.recoveryDirectory / "valid.recovery";
    const auto malformed = paths.recoveryDirectory / "damaged.recovery";
    REQUIRE(SaveTunerRecoveryJournal(
                MakeV5Record(), valid.string()).success);
    WriteText(malformed, "damaged recovery evidence\n");
    const auto validBefore = ReadText(valid);
    const auto malformedBefore = ReadText(malformed);

    const auto discovery = DiscoverApplicationRecovery(paths);
    CHECK(discovery.state == ApplicationRecoveryDiscoveryState::Blocked);
    CHECK(discovery.blockReason
          == ApplicationRecoveryBlockReason::MultipleRecords);
    CHECK_FALSE(discovery.record.has_value());
    CHECK(ReadText(valid) == validBefore);
    CHECK(ReadText(malformed) == malformedBefore);
}
