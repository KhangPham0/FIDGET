#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/ReadoutProtocol.h"
#include "core/RecoveryVerification.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace {

fidget::TunerRecoveryRecord MakeRecord(const std::uint32_t version = 3U)
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

TEST_CASE("DAQ zero declares an already-clean stale journal")
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

TEST_CASE("version 2 and version 3 records use the same wire fingerprint")
{
    using namespace fidget;

    for (const std::uint32_t version : {2U, 3U})
    {
        const auto record = MakeRecord(version);
        const auto evaluated = EvaluateTunerRecoveryFingerprint(
            record, MakeMatchingLive(record));
        INFO(version);
        CHECK(evaluated.verdict
              == TunerRecoveryFingerprintVerdict::OrphanConfirmed);
    }
}
