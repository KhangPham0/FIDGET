#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/ReadoutProtocol.h"
#include "core/RecoveryVerification.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace {

fidget::TunerRecoveryRecord MakeRecord(
    const std::uint32_t version =
        fidget::TunerRecoveryJournalFormatVersion)
{
    fidget::TunerRecoveryRecord record;
    record.formatVersion = version;
    record.phase = fidget::TunerRecoveryPhase::Active;
    record.host = "mvlc-test";
    record.commandPort = 32768U;
    record.mvlcHardwareId = 0x5008U;
    record.mvlcFirmwareRevision = 0x0046U;
    record.mdppBaseAddress = 0x11000000U;
    record.mdppHardwareId = 0x5007U;
    record.mdppIrqLevel = 3U;
    record.mdppOutputFormat = 0x0018U;
    record.stackTriggerRegister = 0x1104U;
    record.stackTriggerValue = 0x0042U;
    record.stackOffsetRegister = 0x1204U;
    record.stackOffsetValue = 0x0200U;
    record.ownershipTokenRegister = 0x221CU;
    record.ownershipTokenValue = 0xA55A1234U;
    return record;
}

void RequireSourceRestoration(fidget::TunerRecoveryRecord& record)
{
    record.sourceRestoreRequired = true;
    record.sourceQuad = 7U;
    record.sourceOriginalConfiguration = 0x0040U;
    if (record.formatVersion >= 4U)
    {
        record.sourceAppliedConfigurationAvailable = true;
        record.sourceAppliedConfiguration = 0x0043U;
    }
}

fidget::TunerRecoveryLiveFingerprint MakeMatchingLive(
    const fidget::TunerRecoveryRecord& record)
{
    using namespace fidget;

    const auto expected = BuildTunerRecoveryFingerprintExpectation(record);
    REQUIRE(expected.success);
    TunerRecoveryLiveFingerprint live;
    live.mvlcHardwareId = record.mvlcHardwareId;
    live.mvlcFirmwareRevision = record.mvlcFirmwareRevision;
    live.values = expected.values;
    live.values[0] = 0x00000005U;
    return live;
}

} // namespace

TEST_CASE("the complete journal fingerprint confirms a tuner-owned orphan")
{
    using namespace fidget;

    const auto record = MakeRecord();
    const auto expected = BuildTunerRecoveryFingerprintExpectation(record);
    REQUIRE(expected.success);
    CHECK(expected.addresses[0] == 0x1300U);
    CHECK(expected.addresses[1] == 0x1104U);
    CHECK(expected.addresses[2] == 0x1204U);
    CHECK(expected.addresses[3] == 0x2200U);
    CHECK(expected.addresses[9] == 0x2218U);
    CHECK(expected.addresses[10] == 0x221CU);

    const auto evaluated = EvaluateTunerRecoveryFingerprint(
        record, MakeMatchingLive(record));
    CHECK(evaluated.verdict
          == TunerRecoveryFingerprintVerdict::OrphanConfirmed);
    CHECK(evaluated.firstMismatchedField.empty());
}

TEST_CASE("identity mismatches name the first foreign field")
{
    using namespace fidget;

    const auto record = MakeRecord();
    auto live = MakeMatchingLive(record);
    live.mvlcHardwareId ^= 1U;
    auto evaluated = EvaluateTunerRecoveryFingerprint(record, live);
    CHECK(evaluated.verdict
          == TunerRecoveryFingerprintVerdict::ForeignOrMismatched);
    CHECK(evaluated.firstMismatchedField == "MVLC hardware ID");

    live = MakeMatchingLive(record);
    live.mvlcFirmwareRevision ^= 1U;
    evaluated = EvaluateTunerRecoveryFingerprint(record, live);
    CHECK(evaluated.verdict
          == TunerRecoveryFingerprintVerdict::ForeignOrMismatched);
    CHECK(evaluated.firstMismatchedField == "MVLC firmware revision");

    auto idlePending = record;
    idlePending.previewRestoreRequired = true;
    live = MakeMatchingLive(idlePending);
    live.values[0] = 0U;
    live.mvlcHardwareId ^= 1U;
    evaluated = EvaluateTunerRecoveryFingerprint(idlePending, live);
    CHECK(evaluated.verdict
          == TunerRecoveryFingerprintVerdict::ForeignOrMismatched);
    CHECK(evaluated.firstMismatchedField == "MVLC hardware ID");
}

TEST_CASE("every owned fingerprint field is checked and named")
{
    using namespace fidget;

    const auto record = MakeRecord();
    const std::array<std::string, 10> names{{
        "Diagnostic stack trigger",
        "Diagnostic stack offset",
        "Diagnostic stack word 0",
        "Diagnostic stack word 1",
        "Diagnostic stack word 2",
        "Diagnostic stack word 3",
        "Diagnostic stack word 4",
        "Diagnostic stack word 5",
        "Diagnostic stack word 6",
        "Unique tuner ownership token",
    }};
    for (std::size_t index = 1U; index < 11U; ++index)
    {
        auto live = MakeMatchingLive(record);
        live.values[index] ^= 1U;
        const auto evaluated = EvaluateTunerRecoveryFingerprint(
            record, live);
        INFO(index);
        CHECK(evaluated.verdict
              == TunerRecoveryFingerprintVerdict::ForeignOrMismatched);
        CHECK(evaluated.firstMismatchedField == names[index - 1U]);
    }
}

TEST_CASE("DAQ zero is already clean only when no restoration is pending")
{
    using namespace fidget;

    const auto record = MakeRecord();
    auto live = MakeMatchingLive(record);
    live.values[0] = 0U;
    const auto evaluated = EvaluateTunerRecoveryFingerprint(record, live);
    CHECK(evaluated.verdict
          == TunerRecoveryFingerprintVerdict::AlreadyClean);
    CHECK(evaluated.message.find("without a hardware write")
          != std::string::npos);
}

TEST_CASE("DAQ zero with a pending preview requires restoration")
{
    using namespace fidget;

    for (const std::uint32_t version : {1U, 4U})
    {
        auto record = MakeRecord(version);
        record.previewRestoreRequired = true;
        record.previewQuad = 7U;
        record.previewRegisterOffset = 0x611AU;
        record.previewOriginalValue = 200U;
        record.previewAppliedValue = 250U;
        auto live = MakeMatchingLive(record);
        live.values[0] = 0U;

        const auto evaluated = EvaluateTunerRecoveryFingerprint(
            record, live);
        INFO(version);
        CHECK(evaluated.verdict
              == TunerRecoveryFingerprintVerdict::IdleWithRestoration);
        CHECK(evaluated.message.find("journal must be retained")
              != std::string::npos);
    }
}

TEST_CASE("DAQ zero with a pending source requires restoration")
{
    using namespace fidget;

    for (const std::uint32_t version : {3U, 4U})
    {
        auto record = MakeRecord(version);
        RequireSourceRestoration(record);
        auto live = MakeMatchingLive(record);
        live.values[0] = 0U;

        const auto evaluated = EvaluateTunerRecoveryFingerprint(
            record, live);
        INFO(version);
        CHECK(evaluated.verdict
              == TunerRecoveryFingerprintVerdict::IdleWithRestoration);
        CHECK(evaluated.message.find("journal must be retained")
              != std::string::npos);
    }
}

TEST_CASE("pending restorations do not change an active orphan verdict")
{
    using namespace fidget;

    auto record = MakeRecord();
    record.previewRestoreRequired = true;
    RequireSourceRestoration(record);

    const auto evaluated = EvaluateTunerRecoveryFingerprint(
        record, MakeMatchingLive(record));
    CHECK(evaluated.verdict
          == TunerRecoveryFingerprintVerdict::OrphanConfirmed);

    auto mismatched = MakeMatchingLive(record);
    mismatched.values.back() ^= 1U;
    const auto rejected = EvaluateTunerRecoveryFingerprint(
        record, mismatched);
    CHECK(rejected.verdict
          == TunerRecoveryFingerprintVerdict::ForeignOrMismatched);
    CHECK(rejected.firstMismatchedField
          == "Unique tuner ownership token");
}

TEST_CASE("versions 1 through 4 use the same wire fingerprint")
{
    using namespace fidget;

    for (const std::uint32_t version : {1U, 2U, 3U, 4U})
    {
        const auto record = MakeRecord(version);
        const auto evaluated = EvaluateTunerRecoveryFingerprint(
            record, MakeMatchingLive(record));
        INFO(version);
        CHECK(evaluated.verdict
              == TunerRecoveryFingerprintVerdict::OrphanConfirmed);
    }
}
