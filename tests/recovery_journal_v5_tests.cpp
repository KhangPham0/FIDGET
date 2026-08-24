#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/RecoveryJournal.h"
#include "core/ScpRegistry.h"

#include <array>
#include <cctype>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr std::uint32_t TargetBase = 0x11000000U;

fidget::ControllerEndpointRequest DirectEndpoint(
    std::string host = "mvlc-test",
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

fidget::Fw2051ScpConfigurationSnapshot MakeSnapshot()
{
    using namespace fidget;

    Fw2051ScpConfigurationSnapshot snapshot;
    snapshot.state = ScpConfigurationState::Complete;
    snapshot.message = "Live restoration authority";
    snapshot.baseAddress = TargetBase;
    snapshot.hardwareId = Mdpp32HardwareId;
    snapshot.firmwareRevision = Mdpp32ScpFirmwareRevisionFw2051;
    snapshot.irqLevel = 1U;
    snapshot.outputFormat = 0x18U;
    snapshot.selectorParkedAtQuadZero = true;
    for (std::uint16_t index = 0U;
         index < Fw2051ScpQuadCount; ++index)
    {
        Fw2051ScpQuadConfiguration quad;
        quad.quad = index;
        quad.timingFilter = static_cast<std::uint16_t>(20U + index);
        for (std::uint16_t channel = 0U; channel < 4U; ++channel)
        {
            quad.poleZero[channel] = static_cast<std::uint16_t>(
                100U + index * 4U + channel);
            quad.thresholds[channel] = static_cast<std::uint16_t>(
                40U + index * 4U + channel);
        }
        quad.gain = static_cast<std::uint16_t>(1000U + index);
        quad.shapingTime = static_cast<std::uint16_t>(160U + index);
        quad.baselineRestorer = static_cast<std::uint16_t>(index % 4U);
        quad.resetTime = static_cast<std::uint16_t>(64U + index);
        quad.signalRiseTime = static_cast<std::uint16_t>(24U + index);
        quad.preSamples = static_cast<std::uint16_t>(32U + index);
        quad.totalSamples = static_cast<std::uint16_t>(200U + index * 2U);
        quad.sampleConfiguration = static_cast<std::uint16_t>(index % 4U);
        snapshot.quads.push_back(quad);
    }
    return snapshot;
}

fidget::TunerRecoveryV5OwnershipEvidence MakeOwnership()
{
    fidget::TunerRecoveryV5OwnershipEvidence ownership;
    ownership.stackTriggerRegister = 0x1104U;
    ownership.stackTriggerValue = 0x40U;
    ownership.stackOffsetRegister = 0x1204U;
    ownership.stackOffsetValue = 0x200U;
    ownership.ownershipTokenRegister = 0x221CU;
    ownership.ownershipTokenValue = 0x1234ABCDU;
    ownership.isolatedModuleBaseAddresses = {
        0x22000000U,
        0x33000000U,
    };
    return ownership;
}

fidget::TunerRecoveryV5Deviation MakeGlobalDeviation(
    const std::uint32_t ordinal,
    const fidget::TuningSessionPhase phase,
    const std::uint16_t previous = 1U,
    const std::uint16_t requested = 2U)
{
    fidget::TunerRecoveryV5Deviation deviation;
    deviation.ordinal = ordinal;
    deviation.scope = fidget::TunerRecoveryDeviationScope::Global;
    deviation.registerOffset = 0x6010U;
    deviation.originalSessionValue = 1U;
    deviation.previousVerifiedWorkingValue = previous;
    deviation.requestedNextValue = requested;
    deviation.transitionPhase = phase;
    return deviation;
}

fidget::TunerRecoveryV5Deviation MakeQuadDeviation(
    const std::uint32_t ordinal,
    const fidget::TuningSessionPhase phase,
    const std::uint16_t previous = 1000U,
    const std::uint16_t requested = 1200U)
{
    fidget::TunerRecoveryV5Deviation deviation;
    deviation.ordinal = ordinal;
    deviation.scope = fidget::TunerRecoveryDeviationScope::Quad;
    deviation.quad = 0U;
    deviation.registerOffset = 0x611AU;
    deviation.originalSessionValue = 1000U;
    deviation.previousVerifiedWorkingValue = previous;
    deviation.requestedNextValue = requested;
    deviation.transitionPhase = phase;
    return deviation;
}

fidget::TunerRecoveryRecord MakeRecord(
    fidget::ControllerEndpointRequest endpoint = DirectEndpoint())
{
    using namespace fidget;

    TunerRecoveryRecord record;
    record.formatVersion = TunerRecoveryJournalV5FormatVersion;
    TunerRecoveryV5Data data;
    data.sessionPhase = TuningSessionPhase::Manual;
    data.endpoint = std::move(endpoint);
    data.identity.mvlcHardwareId = 0x5008U;
    data.identity.mvlcFirmwareRevision = 0x0046U;
    data.identity.targetBaseAddress = TargetBase;
    data.identity.targetHardwareId = Mdpp32HardwareId;
    data.identity.targetFirmwareRevision =
        Mdpp32ScpFirmwareRevisionFw2051;
    data.ownership = MakeOwnership();
    data.liveRestoreSnapshot = MakeSnapshot();
    data.deviations = {
        MakeGlobalDeviation(0U, TuningSessionPhase::AutomaticEnergy),
        MakeQuadDeviation(1U, TuningSessionPhase::Manual),
    };
    record.version5 = std::move(data);
    return record;
}

const fidget::TunerRecoveryV5Data& RoundTrip(
    const fidget::TunerRecoveryRecord& record)
{
    static fidget::TunerRecoveryParseResult parsed;
    const auto serialized = fidget::SerializeTunerRecoveryJournal(record);
    INFO(serialized.message);
    REQUIRE(serialized.success);
    parsed = fidget::ParseTunerRecoveryJournal(serialized.text);
    INFO(parsed.message);
    REQUIRE(parsed.success);
    REQUIRE(parsed.record.has_value());
    REQUIRE(parsed.record->version5.has_value());
    const auto reserialized =
        fidget::SerializeTunerRecoveryJournal(*parsed.record);
    INFO(reserialized.message);
    REQUIRE(reserialized.success);
    CHECK(reserialized.text == serialized.text);
    return *parsed.record->version5;
}

void CheckQuadEqual(
    const fidget::Fw2051ScpQuadConfiguration& left,
    const fidget::Fw2051ScpQuadConfiguration& right)
{
    CHECK(left.quad == right.quad);
    CHECK(left.timingFilter == right.timingFilter);
    CHECK(left.poleZero == right.poleZero);
    CHECK(left.gain == right.gain);
    CHECK(left.thresholds == right.thresholds);
    CHECK(left.shapingTime == right.shapingTime);
    CHECK(left.baselineRestorer == right.baselineRestorer);
    CHECK(left.resetTime == right.resetTime);
    CHECK(left.signalRiseTime == right.signalRiseTime);
    CHECK(left.preSamples == right.preSamples);
    CHECK(left.totalSamples == right.totalSamples);
    CHECK(left.sampleConfiguration == right.sampleConfiguration);
}

void CheckSnapshotEqual(
    const fidget::Fw2051ScpConfigurationSnapshot& left,
    const fidget::Fw2051ScpConfigurationSnapshot& right)
{
    CHECK(left.state == right.state);
    CHECK(left.baseAddress == right.baseAddress);
    CHECK(left.hardwareId == right.hardwareId);
    CHECK(left.firmwareRevision == right.firmwareRevision);
    CHECK(left.irqLevel == right.irqLevel);
    CHECK(left.outputFormat == right.outputFormat);
    CHECK(left.selectorParkedAtQuadZero
          == right.selectorParkedAtQuadZero);
    REQUIRE(left.quads.size() == right.quads.size());
    for (std::size_t index = 0U; index < left.quads.size(); ++index)
        CheckQuadEqual(left.quads[index], right.quads[index]);
}

void ReplaceOnce(
    std::string& text,
    const std::string& from,
    const std::string& to)
{
    const auto position = text.find(from);
    REQUIRE(position != std::string::npos);
    text.replace(position, from.size(), to);
}

std::string Lowercase(std::string value)
{
    for (char& character : value)
    {
        character = static_cast<char>(std::tolower(
            static_cast<unsigned char>(character)));
    }
    return value;
}

constexpr std::array<fidget::TuningSessionPhase, 12U> SessionPhases{{
    fidget::TuningSessionPhase::Home,
    fidget::TuningSessionPhase::Preparing,
    fidget::TuningSessionPhase::Goal,
    fidget::TuningSessionPhase::Group,
    fidget::TuningSessionPhase::AutomaticLearnSignal,
    fidget::TuningSessionPhase::AutomaticEnergy,
    fidget::TuningSessionPhase::AutomaticTiming,
    fidget::TuningSessionPhase::Manual,
    fidget::TuningSessionPhase::GroupResult,
    fidget::TuningSessionPhase::Restoring,
    fidget::TuningSessionPhase::NextGroup,
    fidget::TuningSessionPhase::Finished,
}};

} // namespace

TEST_CASE("v5 identity-only Prepared journals round-trip without inventing a snapshot")
{
    using namespace fidget;

    auto record = MakeRecord();
    record.version5->sessionPhase = TuningSessionPhase::Preparing;
    record.version5->selectorParkingRequired = true;
    record.version5->ownership.reset();
    record.version5->liveRestoreSnapshot.reset();
    record.version5->deviations.clear();

    const auto& parsed = RoundTrip(record);
    CHECK(parsed.sessionPhase == TuningSessionPhase::Preparing);
    CHECK(parsed.selectorParkingRequired);
    CHECK_FALSE(parsed.ownership.has_value());
    CHECK_FALSE(parsed.liveRestoreSnapshot.has_value());
    CHECK(parsed.deviations.empty());
}

TEST_CASE("v5 complete snapshots preserve every global and banked value")
{
    using namespace fidget;

    const auto record = MakeRecord();
    const auto& parsed = RoundTrip(record);
    REQUIRE(parsed.liveRestoreSnapshot.has_value());
    CheckSnapshotEqual(
        *record.version5->liveRestoreSnapshot,
        *parsed.liveRestoreSnapshot);
    REQUIRE(parsed.ownership.has_value());
    CHECK(parsed.ownership->isolatedModuleBaseAddresses
          == record.version5->ownership->isolatedModuleBaseAddresses);
    REQUIRE(parsed.deviations.size() == 2U);
    CHECK(parsed.deviations[0U].scope
          == TunerRecoveryDeviationScope::Global);
    CHECK_FALSE(parsed.deviations[0U].quad.has_value());
    CHECK(parsed.deviations[1U].scope
          == TunerRecoveryDeviationScope::Quad);
    CHECK(parsed.deviations[1U].quad == 0U);
}

TEST_CASE("v5 snapshots preserve aligned target address zero without a sentinel")
{
    auto record = MakeRecord();
    record.version5->identity.targetBaseAddress = 0U;
    record.version5->liveRestoreSnapshot->baseAddress = 0U;

    const auto& parsed = RoundTrip(record);
    CHECK(parsed.identity.targetBaseAddress == 0U);
    REQUIRE(parsed.liveRestoreSnapshot.has_value());
    CHECK(parsed.liveRestoreSnapshot->baseAddress == 0U);
}

TEST_CASE("v5 snapshots preserve maximal raw D16 restoration values")
{
    auto record = MakeRecord();
    auto& snapshot = *record.version5->liveRestoreSnapshot;
    snapshot.irqLevel = 0xFFFFU;
    snapshot.outputFormat = 0xFFFFU;
    for (auto& quad : snapshot.quads)
    {
        quad.timingFilter = 0xFFFFU;
        quad.poleZero.fill(0xFFFFU);
        quad.gain = 0xFFFFU;
        quad.thresholds.fill(0xFFFFU);
        quad.shapingTime = 0xFFFFU;
        quad.baselineRestorer = 0xFFFFU;
        quad.resetTime = 0xFFFFU;
        quad.signalRiseTime = 0xFFFFU;
        quad.preSamples = 0xFFFFU;
        quad.totalSamples = 0xFFFFU;
        quad.sampleConfiguration = 0xFFFFU;
    }
    record.version5->deviations.clear();

    const auto& parsed = RoundTrip(record);
    REQUIRE(parsed.liveRestoreSnapshot.has_value());
    CheckSnapshotEqual(snapshot, *parsed.liveRestoreSnapshot);
}

TEST_CASE("v5 round-trips every session and transition phase")
{
    using namespace fidget;

    for (const auto phase : SessionPhases)
    {
        CAPTURE(static_cast<int>(phase));
        auto record = MakeRecord();
        record.version5->sessionPhase = phase;
        record.version5->deviations = {
            MakeGlobalDeviation(0U, phase),
        };
        const auto& parsed = RoundTrip(record);
        CHECK(parsed.sessionPhase == phase);
        REQUIRE(parsed.deviations.size() == 1U);
        CHECK(parsed.deviations.front().transitionPhase == phase);
    }
}

TEST_CASE("v5 deviation chains support empty repeated and maximal ledgers")
{
    using namespace fidget;

    auto empty = MakeRecord();
    empty.version5->deviations.clear();
    CHECK(RoundTrip(empty).deviations.empty());

    auto repeated = MakeRecord();
    repeated.version5->deviations = {
        MakeQuadDeviation(0U, TuningSessionPhase::AutomaticEnergy,
                          1000U, 1200U),
        MakeQuadDeviation(1U, TuningSessionPhase::Manual,
                          1200U, 1300U),
        MakeQuadDeviation(2U, TuningSessionPhase::Restoring,
                          1300U, 1000U),
    };
    const auto& repeatedParsed = RoundTrip(repeated);
    REQUIRE(repeatedParsed.deviations.size() == 3U);
    CHECK(repeatedParsed.deviations.back().requestedNextValue == 1000U);

    auto maximal = MakeRecord();
    maximal.version5->deviations.clear();
    std::uint16_t previous = 1000U;
    for (std::size_t index = 0U;
         index < TunerRecoveryV5MaximumDeviationCount; ++index)
    {
        const std::uint16_t requested = previous == 1000U
            ? 1001U
            : 1000U;
        maximal.version5->deviations.push_back(MakeQuadDeviation(
            static_cast<std::uint32_t>(index),
            TuningSessionPhase::AutomaticEnergy,
            previous,
            requested));
        previous = requested;
    }
    const auto& maximalParsed = RoundTrip(maximal);
    CHECK(maximalParsed.deviations.size()
          == TunerRecoveryV5MaximumDeviationCount);
    CHECK(maximalParsed.deviations.back().ordinal
          == TunerRecoveryV5MaximumDeviationCount - 1U);
}

TEST_CASE("v5 direct and SSH endpoints carry routing fields and no secrets")
{
    using namespace fidget;

    auto direct = MakeRecord(DirectEndpoint("controller.example", 32768U));
    const auto directSerialized = SerializeTunerRecoveryJournal(direct);
    REQUIRE(directSerialized.success);
    const auto directParsed = ParseTunerRecoveryJournal(directSerialized.text);
    REQUIRE(directParsed.success);
    const auto& directEndpoint = directParsed.record->version5->endpoint;
    CHECK(directEndpoint.kind == ControllerEndpointKind::DirectEthernet);
    CHECK(directEndpoint.mvlcHost == "controller.example");
    CHECK(directEndpoint.mvlcCommandPort == 32768U);

    auto bridge = MakeRecord(BridgeEndpoint(
        "controller.example",
        41000U,
        "bridge-alias",
        "/opt/fidget/fidget_bridge"));
    const auto bridgeSerialized = SerializeTunerRecoveryJournal(bridge);
    INFO(bridgeSerialized.message);
    REQUIRE(bridgeSerialized.success);
    const auto bridgeParsed = ParseTunerRecoveryJournal(bridgeSerialized.text);
    INFO(bridgeParsed.message);
    REQUIRE(bridgeParsed.success);
    const auto& bridgeEndpoint = bridgeParsed.record->version5->endpoint;
    CHECK(bridgeEndpoint.kind == ControllerEndpointKind::SshBridge);
    CHECK(bridgeEndpoint.mvlcHost == "controller.example");
    CHECK(bridgeEndpoint.mvlcCommandPort == 41000U);
    CHECK(bridgeEndpoint.sshDestination == "bridge-alias");
    CHECK(bridgeEndpoint.remoteBridgeCommand
          == "/opt/fidget/fidget_bridge");

    const auto lower = Lowercase(bridgeSerialized.text);
    CHECK(lower.find("password") == std::string::npos);
    CHECK(lower.find("private_key") == std::string::npos);
    CHECK(lower.find("secret") == std::string::npos);
    CHECK(lower.find("token") == std::string::npos);
}

TEST_CASE("v5 section and record checksums reject byte-level corruption")
{
    using namespace fidget;

    auto record = MakeRecord(BridgeEndpoint(
        "mvlc-test", 32768U, "bridge-a", "/opt/fidget_bridge"));
    const auto serialized = SerializeTunerRecoveryJournal(record);
    REQUIRE(serialized.success);

    struct Corruption
    {
        const char* from;
        const char* to;
        const char* section;
    };
    const std::array<Corruption, 6U> corruptions{{
        {"SESSION MANUAL 0", "SESSION GROUP 0", "session"},
        {"SSH_BRIDGE mvlc-test", "SSH_BRIDGE mvlc-best", "endpoint"},
        {"IDENTITY 20488 70", "IDENTITY 20488 71", "identity"},
        {"OWNERSHIP 1 4356 64", "OWNERSHIP 1 4356 65", "ownership"},
        {"GLOBALS 285212672 20487 8273 1 24",
         "GLOBALS 285212672 20487 8273 2 24", "snapshot"},
        {"DEVIATION 1 QUAD 0 24858 1000 1000 1200 MANUAL",
         "DEVIATION 1 QUAD 0 24858 1000 1000 1201 MANUAL",
         "deviations"},
    }};

    for (const auto& corruption : corruptions)
    {
        CAPTURE(corruption.section);
        auto text = serialized.text;
        ReplaceOnce(text, corruption.from, corruption.to);
        const auto parsed = ParseTunerRecoveryJournal(text);
        CHECK_FALSE(parsed.success);
        CHECK(parsed.message.find("checksum mismatch")
              != std::string::npos);
    }

    auto recordChecksum = serialized.text;
    const auto marker = recordChecksum.find("RECORD_CHECKSUM ");
    REQUIRE(marker != std::string::npos);
    const auto checksumEnd = recordChecksum.find('\n', marker);
    REQUIRE(checksumEnd != std::string::npos);
    REQUIRE(checksumEnd > marker);
    const auto digit = checksumEnd - 1U;
    recordChecksum[digit] = recordChecksum[digit] == '9'
        ? '8'
        : static_cast<char>(recordChecksum[digit] + 1);
    const auto parsed = ParseTunerRecoveryJournal(recordChecksum);
    INFO(parsed.message);
    CHECK_FALSE(parsed.success);
    CHECK(parsed.message.find("record checksum mismatch")
          != std::string::npos);
}

TEST_CASE("v5 truncation version errors unknown phases and disorder fail closed")
{
    using namespace fidget;

    const auto serialized = SerializeTunerRecoveryJournal(MakeRecord());
    REQUIRE(serialized.success);

    const std::array<const char*, 6U> sectionMarkers{{
        "SESSION ",
        "ENDPOINT ",
        "IDENTITY ",
        "OWNERSHIP ",
        "RESTORE_SNAPSHOT ",
        "DEVIATIONS ",
    }};
    for (const auto* section : sectionMarkers)
    {
        CAPTURE(section);
        const auto position = serialized.text.find(section);
        REQUIRE(position != std::string::npos);
        const auto truncated = serialized.text.substr(
            0U, position + std::string(section).size() + 1U);
        CHECK_FALSE(ParseTunerRecoveryJournal(truncated).success);
    }

    auto declaredV4 = serialized.text;
    ReplaceOnce(declaredV4, "MWW_TUNER_RECOVERY 5",
                 "MWW_TUNER_RECOVERY 4");
    CHECK_FALSE(ParseTunerRecoveryJournal(declaredV4).success);

    auto declaredV6 = serialized.text;
    ReplaceOnce(declaredV6, "MWW_TUNER_RECOVERY 5",
                 "MWW_TUNER_RECOVERY 6");
    CHECK_FALSE(ParseTunerRecoveryJournal(declaredV6).success);

    auto unknownSession = serialized.text;
    ReplaceOnce(unknownSession, "SESSION MANUAL",
                 "SESSION UNKNOWN_PHASE");
    const auto unknownSessionResult =
        ParseTunerRecoveryJournal(unknownSession);
    CHECK_FALSE(unknownSessionResult.success);
    CHECK(unknownSessionResult.message.find("Unknown session phase")
          != std::string::npos);

    auto unknownTransition = serialized.text;
    ReplaceOnce(unknownTransition,
                 "1200 MANUAL", "1200 UNKNOWN_PHASE");
    const auto unknownTransitionResult =
        ParseTunerRecoveryJournal(unknownTransition);
    CHECK_FALSE(unknownTransitionResult.success);
    CHECK(unknownTransitionResult.message.find("Unknown deviation phase")
          != std::string::npos);

    auto outOfOrder = serialized.text;
    ReplaceOnce(outOfOrder,
                 "DEVIATION 1 QUAD", "DEVIATION 0 QUAD");
    const auto outOfOrderResult = ParseTunerRecoveryJournal(outOfOrder);
    CHECK_FALSE(outOfOrderResult.success);
    CHECK(outOfOrderResult.message.find("out of order")
          != std::string::npos);
}

TEST_CASE("v5 validation rejects incomplete snapshots and inconsistent ledgers")
{
    using namespace fidget;

    auto noSnapshot = MakeRecord();
    noSnapshot.version5->liveRestoreSnapshot.reset();
    noSnapshot.version5->deviations.clear();
    CHECK_FALSE(SerializeTunerRecoveryJournal(noSnapshot).success);

    auto incomplete = MakeRecord();
    incomplete.version5->liveRestoreSnapshot->quads.pop_back();
    CHECK_FALSE(SerializeTunerRecoveryJournal(incomplete).success);

    auto badScope = MakeRecord();
    badScope.version5->deviations.front().scope =
        static_cast<TunerRecoveryDeviationScope>(99U);
    CHECK_FALSE(SerializeTunerRecoveryJournal(badScope).success);

    auto badRegister = MakeRecord();
    badRegister.version5->deviations.front().registerOffset = 0x603AU;
    CHECK_FALSE(SerializeTunerRecoveryJournal(badRegister).success);

    auto wrongOriginal = MakeRecord();
    wrongOriginal.version5->deviations.front().originalSessionValue = 2U;
    CHECK_FALSE(SerializeTunerRecoveryJournal(wrongOriginal).success);

    auto brokenChain = MakeRecord();
    brokenChain.version5->deviations = {
        MakeQuadDeviation(0U, TuningSessionPhase::AutomaticEnergy,
                          1000U, 1200U),
        MakeQuadDeviation(1U, TuningSessionPhase::Manual,
                          1199U, 1300U),
    };
    CHECK_FALSE(SerializeTunerRecoveryJournal(brokenChain).success);

    auto tooMany = MakeRecord();
    tooMany.version5->deviations.clear();
    std::uint16_t previous = 1000U;
    for (std::size_t index = 0U;
         index <= TunerRecoveryV5MaximumDeviationCount; ++index)
    {
        const auto requested = static_cast<std::uint16_t>(
            previous == 1000U ? 1001U : 1000U);
        tooMany.version5->deviations.push_back(MakeQuadDeviation(
            static_cast<std::uint32_t>(index),
            TuningSessionPhase::Manual,
            previous,
            requested));
        previous = requested;
    }
    CHECK_FALSE(SerializeTunerRecoveryJournal(tooMany).success);
}

TEST_CASE("v5 acceptable live values have explicit non-conflated actions")
{
    using namespace fidget;

    auto first = MakeQuadDeviation(
        0U, TuningSessionPhase::AutomaticEnergy, 1000U, 1200U);
    const auto evidence = TunerRecoveryV5EvidenceDefinedValues(first);
    CHECK(evidence[0U].evidence
          == TunerRecoveryLiveValueEvidence::OriginalSession);
    CHECK(evidence[0U].action
          == TunerRecoveryLiveValueAction::NoWriteAlreadyOriginal);
    CHECK(evidence[1U].evidence
          == TunerRecoveryLiveValueEvidence::PreviousVerifiedWorking);
    CHECK(evidence[1U].action
          == TunerRecoveryLiveValueAction::RestoreOriginal);
    CHECK(evidence[2U].evidence
          == TunerRecoveryLiveValueEvidence::RequestedNext);
    CHECK(evidence[2U].action
          == TunerRecoveryLiveValueAction::RestoreOriginal);

    auto decision = ClassifyTunerRecoveryV5LiveValue(first, 1000U);
    CHECK(decision.evidence
          == TunerRecoveryLiveValueEvidence::OriginalSession);
    CHECK(decision.action
          == TunerRecoveryLiveValueAction::NoWriteAlreadyOriginal);

    decision = ClassifyTunerRecoveryV5LiveValue(first, 1200U);
    CHECK(decision.evidence
          == TunerRecoveryLiveValueEvidence::RequestedNext);
    CHECK(decision.action
          == TunerRecoveryLiveValueAction::RestoreOriginal);

    auto later = MakeQuadDeviation(
        1U, TuningSessionPhase::Manual, 1200U, 1300U);
    decision = ClassifyTunerRecoveryV5LiveValue(later, 1200U);
    CHECK(decision.evidence
          == TunerRecoveryLiveValueEvidence::PreviousVerifiedWorking);
    CHECK(decision.action
          == TunerRecoveryLiveValueAction::RestoreOriginal);

    decision = ClassifyTunerRecoveryV5LiveValue(later, 1250U);
    CHECK(decision.evidence == TunerRecoveryLiveValueEvidence::None);
    CHECK(decision.action
          == TunerRecoveryLiveValueAction::BlockAndRetain);

    auto requestedOriginal = MakeQuadDeviation(
        2U, TuningSessionPhase::Restoring, 1300U, 1000U);
    decision = ClassifyTunerRecoveryV5LiveValue(
        requestedOriginal, 1000U);
    CHECK(decision.evidence
          == TunerRecoveryLiveValueEvidence::OriginalSession);
    CHECK(decision.action
          == TunerRecoveryLiveValueAction::NoWriteAlreadyOriginal);
}

TEST_CASE("legacy writer default stays v4 while v5 requires explicit opt in")
{
    using namespace fidget;

    CHECK(TunerRecoveryJournalFormatVersion == 4U);
    CHECK(TunerRecoveryJournalV5FormatVersion == 5U);
    CHECK(TunerRecoveryRecord{}.formatVersion == 4U);

    auto missingV5 = TunerRecoveryRecord{};
    missingV5.formatVersion = TunerRecoveryJournalV5FormatVersion;
    CHECK_FALSE(SerializeTunerRecoveryJournal(missingV5).success);

    auto mixed = MakeRecord();
    mixed.host = "legacy-host";
    CHECK_FALSE(SerializeTunerRecoveryJournal(mixed).success);
}
