#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/RecoveryJournal.h"

#include <chrono>
#include <cstdlib>
#include <string>

namespace {

fidget::TunerRecoveryRecord MakeRecord()
{
    using namespace fidget;

    TunerRecoveryRecord record;
    record.phase = TunerRecoveryPhase::Active;
    record.host = "mvlc-test";
    record.commandPort = 32768U;
    record.mvlcHardwareId = 0x5008U;
    record.mvlcFirmwareRevision = 0x0046U;
    record.mdppBaseAddress = 0x11000000U;
    record.mdppHardwareId = 0x5007U;
    record.mdppIrqLevel = 1U;
    record.mdppOutputFormat = 0x18U;
    record.stackTriggerRegister = 0x1104U;
    record.stackTriggerValue = 0x40U;
    record.stackOffsetRegister = 0x1204U;
    record.stackOffsetValue = 0x200U;
    record.ownershipTokenRegister = 0x221CU;
    record.ownershipTokenValue = 0x1234ABCDU;
    record.isolatedModuleBaseAddresses = {0x33330000U, 0x44440000U};
    record.sourceRestoreRequired = true;
    record.sourceQuad = 7U;
    record.sourceOriginalConfiguration = 0x0040U;
    record.sourceAppliedConfigurationAvailable = true;
    record.sourceAppliedConfiguration = 0x0043U;
    record.previewRestoreRequired = true;
    record.previewQuad = 7U;
    record.previewRegisterOffset = 0x611AU;
    record.previewOriginalValue = 200U;
    record.previewAppliedValue = 510U;
    return record;
}

std::string MakeTemporaryPath()
{
    const char* temporaryDirectory = std::getenv("TMPDIR");
    if (temporaryDirectory == nullptr || temporaryDirectory[0] == '\0')
    {
        temporaryDirectory = "/tmp";
    }

    const auto unique = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    return std::string(temporaryDirectory)
        + "/fidget-recovery-journal-" + std::to_string(unique);
}

constexpr const char* LegacyV1Journal =
    "MWW_TUNER_RECOVERY 1\n"
    "ENDPOINT mvlc-test 32768\n"
    "MVLC 20488 70\n"
    "MDPP 285212672 20487 1 24\n"
    "STACK 4356 64 4612 512 8732 305441741\n"
    "STATE 2 1 7 24858 200 510\n"
    "CHECKSUM 4158629254257455462\n"
    "END\n";

constexpr const char* LegacyV2Journal =
    "MWW_TUNER_RECOVERY 2\n"
    "ENDPOINT mvlc-test 32768\n"
    "MVLC 20488 70\n"
    "MDPP 285212672 20487 1 24\n"
    "ISOLATED 2 858980352 1145307136\n"
    "STACK 4356 64 4612 512 8732 305441741\n"
    "STATE 2 1 7 24858 200 510\n"
    "CHECKSUM 1388001408713401195\n"
    "END\n";

constexpr const char* LegacyV3Journal =
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

constexpr const char* CurrentV4Journal =
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

TEST_CASE("recovery journal strings round-trip every owned value")
{
    using namespace fidget;

    const auto record = MakeRecord();
    const auto serialized = SerializeTunerRecoveryJournal(record);
    INFO(serialized.message);
    REQUIRE(serialized.success);
    CHECK(serialized.text == CurrentV4Journal);

    const auto parsed = ParseTunerRecoveryJournal(serialized.text);
    INFO(parsed.message);
    REQUIRE(parsed.success);
    REQUIRE(parsed.record.has_value());
    const auto& roundTrip = *parsed.record;
    CHECK(roundTrip.host == record.host);
    CHECK(roundTrip.commandPort == record.commandPort);
    CHECK(roundTrip.ownershipTokenValue == record.ownershipTokenValue);
    CHECK(roundTrip.isolatedModuleBaseAddresses
          == record.isolatedModuleBaseAddresses);
    CHECK(roundTrip.sourceRestoreRequired);
    CHECK(roundTrip.sourceQuad == 7U);
    CHECK(roundTrip.sourceOriginalConfiguration == 0x0040U);
    CHECK(roundTrip.sourceAppliedConfigurationAvailable);
    CHECK(roundTrip.sourceAppliedConfiguration == 0x0043U);
    CHECK(roundTrip.previewRestoreRequired);
    CHECK(roundTrip.previewOriginalValue == 200U);
    CHECK(roundTrip.previewAppliedValue == 510U);
}

TEST_CASE("recovery journals are adjacent to their crate project")
{
    using namespace fidget;

    CHECK(ProjectTunerRecoveryJournalPath("two-scp-crate.mwwcrate")
          == "two-scp-crate.mwwcrate.recovery");
    CHECK(ProjectTunerRecoveryJournalPath(
              "/data/crates/two-scp-crate.mwwcrate")
          == "/data/crates/two-scp-crate.mwwcrate.recovery");
    CHECK(ProjectTunerRecoveryJournalPath("").empty());
}

TEST_CASE("recovery journal parsing rejects corruption and incomplete records")
{
    using namespace fidget;

    const auto serialized = SerializeTunerRecoveryJournal(MakeRecord());
    REQUIRE(serialized.success);

    CHECK_FALSE(ParseTunerRecoveryJournal(
        serialized.text + "CORRUPTION\n").success);

    auto checksumCorruption = serialized.text;
    const auto hostPosition = checksumCorruption.find("mvlc-test");
    REQUIRE(hostPosition != std::string::npos);
    checksumCorruption.replace(hostPosition, 9U, "mvlc-best");
    const auto corruptResult = ParseTunerRecoveryJournal(checksumCorruption);
    CHECK_FALSE(corruptResult.success);
    CHECK(corruptResult.message.find("checksum mismatch")
          != std::string::npos);

    auto appliedCorruption = serialized.text;
    const auto sourcePosition = appliedCorruption.find("SOURCE 1 7 64 67");
    REQUIRE(sourcePosition != std::string::npos);
    appliedCorruption.replace(
        sourcePosition, std::string("SOURCE 1 7 64 67").size(),
        "SOURCE 1 7 64 66");
    const auto appliedCorruptResult =
        ParseTunerRecoveryJournal(appliedCorruption);
    CHECK_FALSE(appliedCorruptResult.success);
    CHECK(appliedCorruptResult.message.find("checksum mismatch")
          != std::string::npos);

    const auto checksumPosition = serialized.text.find("CHECKSUM");
    REQUIRE(checksumPosition != std::string::npos);
    CHECK_FALSE(ParseTunerRecoveryJournal(
        serialized.text.substr(0U, checksumPosition)).success);
}

TEST_CASE("literal legacy v1 recovery journals remain readable")
{
    using namespace fidget;

    const auto parsed = ParseTunerRecoveryJournal(LegacyV1Journal);
    INFO(parsed.message);
    REQUIRE(parsed.success);
    REQUIRE(parsed.record.has_value());
    CHECK(parsed.record->formatVersion == 1U);
    CHECK(parsed.record->isolatedModuleBaseAddresses.empty());
    CHECK(parsed.record->mvlcFirmwareRevision == 0x0046U);
    CHECK(parsed.record->ownershipTokenValue == 0x1234ABCDU);
    CHECK_FALSE(parsed.record->sourceRestoreRequired);
    CHECK_FALSE(parsed.record->sourceAppliedConfigurationAvailable);
}

TEST_CASE("version 2 recovery journals import without a source deviation")
{
    using namespace fidget;

    const auto parsed = ParseTunerRecoveryJournal(LegacyV2Journal);
    INFO(parsed.message);
    REQUIRE(parsed.success);
    REQUIRE(parsed.record.has_value());
    CHECK(parsed.record->formatVersion == 2U);
    CHECK_FALSE(parsed.record->sourceRestoreRequired);
    CHECK(parsed.record->sourceQuad == 0U);
    CHECK(parsed.record->sourceOriginalConfiguration == 0U);
    CHECK_FALSE(parsed.record->sourceAppliedConfigurationAvailable);
}

TEST_CASE("version 3 recovery journals import without guessed source evidence")
{
    using namespace fidget;

    const auto parsed = ParseTunerRecoveryJournal(LegacyV3Journal);
    INFO(parsed.message);
    REQUIRE(parsed.success);
    REQUIRE(parsed.record.has_value());
    CHECK(parsed.record->formatVersion == 3U);
    CHECK(parsed.record->sourceRestoreRequired);
    CHECK(parsed.record->sourceQuad == 7U);
    CHECK(parsed.record->sourceOriginalConfiguration == 0x0040U);
    CHECK_FALSE(parsed.record->sourceAppliedConfigurationAvailable);
    CHECK(parsed.record->sourceAppliedConfiguration == 0U);

    const auto reserialized =
        SerializeTunerRecoveryJournal(*parsed.record);
    INFO(reserialized.message);
    REQUIRE(reserialized.success);
    CHECK(reserialized.text == LegacyV3Journal);
}

TEST_CASE("version 4 source recovery refuses missing applied evidence")
{
    using namespace fidget;

    auto record = MakeRecord();
    record.sourceAppliedConfigurationAvailable = false;
    const auto serialized = SerializeTunerRecoveryJournal(record);
    CHECK_FALSE(serialized.success);
    CHECK(serialized.message.find("lacks its applied configuration")
          != std::string::npos);

    record = MakeRecord();
    record.sourceAppliedConfiguration =
        record.sourceOriginalConfiguration;
    const auto unchanged = SerializeTunerRecoveryJournal(record);
    CHECK_FALSE(unchanged.success);
    CHECK(unchanged.message.find("inconsistent with its original")
          != std::string::npos);

    record = MakeRecord();
    record.sourceAppliedConfiguration = 0x00C3U;
    const auto changedNonSourceBits =
        SerializeTunerRecoveryJournal(record);
    CHECK_FALSE(changedNonSourceBits.success);
    CHECK(changedNonSourceBits.message.find(
              "inconsistent with its original")
          != std::string::npos);
}

TEST_CASE("recovery journal file wrappers save load remove and report missing")
{
    using namespace fidget;

    const auto path = MakeTemporaryPath();
    std::string removeError;
    REQUIRE(RemoveTunerRecoveryJournal(path, removeError));

    const auto saved = SaveTunerRecoveryJournal(MakeRecord(), path);
    INFO(saved.message);
    REQUIRE(saved.success);

    const auto loaded = LoadTunerRecoveryJournal(path);
    INFO(loaded.message);
    REQUIRE(loaded.success);
    REQUIRE(loaded.record.has_value());
    CHECK(loaded.record->isolatedModuleBaseAddresses.size() == 2U);

    REQUIRE(RemoveTunerRecoveryJournal(path, removeError));
    const auto missing = LoadTunerRecoveryJournal(path);
    CHECK_FALSE(missing.success);
    CHECK(missing.fileMissing);
}
