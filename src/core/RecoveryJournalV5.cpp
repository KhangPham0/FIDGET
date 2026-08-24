#include "core/RecoveryJournalV5.h"

#include "core/ScpRegistry.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <limits>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace fidget {
namespace {

constexpr std::string_view JournalMagic = "MWW_TUNER_RECOVERY";
constexpr std::uint64_t FnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t FnvPrime = 1099511628211ULL;
constexpr std::uint32_t ExpectedMvlcHardwareId = 0x5008U;
constexpr std::uint32_t ExpectedMvlcFirmwareRevision = 0x0046U;
constexpr std::uint16_t IrqLevelRegister = 0x6010U;
constexpr std::uint16_t OutputFormatRegister = 0x6044U;
constexpr std::size_t MaximumIsolatedModuleCount = 15U;

void HashByte(std::uint64_t& hash, const std::uint8_t value)
{
    hash ^= value;
    hash *= FnvPrime;
}

void HashValue(std::uint64_t& hash, const std::uint32_t value)
{
    for (unsigned shift = 0U; shift < 32U; shift += 8U)
        HashByte(hash, static_cast<std::uint8_t>(value >> shift));
}

void HashWideValue(std::uint64_t& hash, const std::uint64_t value)
{
    HashValue(hash, static_cast<std::uint32_t>(value));
    HashValue(hash, static_cast<std::uint32_t>(value >> 32U));
}

void HashText(std::uint64_t& hash, const std::string_view value)
{
    HashValue(hash, static_cast<std::uint32_t>(value.size()));
    for (const unsigned char byte : value)
        HashByte(hash, byte);
}

std::uint32_t SessionPhaseCode(const TuningSessionPhase phase) noexcept
{
    switch (phase)
    {
    case TuningSessionPhase::Home: return 1U;
    case TuningSessionPhase::Preparing: return 2U;
    case TuningSessionPhase::Goal: return 3U;
    case TuningSessionPhase::Group: return 4U;
    case TuningSessionPhase::AutomaticLearnSignal: return 5U;
    case TuningSessionPhase::AutomaticEnergy: return 6U;
    case TuningSessionPhase::AutomaticTiming: return 7U;
    case TuningSessionPhase::Manual: return 8U;
    case TuningSessionPhase::GroupResult: return 9U;
    case TuningSessionPhase::Restoring: return 10U;
    case TuningSessionPhase::NextGroup: return 11U;
    case TuningSessionPhase::Finished: return 12U;
    }
    return 0U;
}

const char* SessionPhaseToken(const TuningSessionPhase phase) noexcept
{
    switch (phase)
    {
    case TuningSessionPhase::Home: return "HOME";
    case TuningSessionPhase::Preparing: return "PREPARING";
    case TuningSessionPhase::Goal: return "GOAL";
    case TuningSessionPhase::Group: return "GROUP";
    case TuningSessionPhase::AutomaticLearnSignal:
        return "AUTOMATIC_LEARN_SIGNAL";
    case TuningSessionPhase::AutomaticEnergy: return "AUTOMATIC_ENERGY";
    case TuningSessionPhase::AutomaticTiming: return "AUTOMATIC_TIMING";
    case TuningSessionPhase::Manual: return "MANUAL";
    case TuningSessionPhase::GroupResult: return "GROUP_RESULT";
    case TuningSessionPhase::Restoring: return "RESTORING";
    case TuningSessionPhase::NextGroup: return "NEXT_GROUP";
    case TuningSessionPhase::Finished: return "FINISHED";
    }
    return nullptr;
}

bool ParseSessionPhase(
    const std::string& token,
    TuningSessionPhase& phase) noexcept
{
    static constexpr TuningSessionPhase Phases[] = {
        TuningSessionPhase::Home,
        TuningSessionPhase::Preparing,
        TuningSessionPhase::Goal,
        TuningSessionPhase::Group,
        TuningSessionPhase::AutomaticLearnSignal,
        TuningSessionPhase::AutomaticEnergy,
        TuningSessionPhase::AutomaticTiming,
        TuningSessionPhase::Manual,
        TuningSessionPhase::GroupResult,
        TuningSessionPhase::Restoring,
        TuningSessionPhase::NextGroup,
        TuningSessionPhase::Finished,
    };
    for (const auto candidate : Phases)
    {
        if (token == SessionPhaseToken(candidate))
        {
            phase = candidate;
            return true;
        }
    }
    return false;
}

bool ValidateEndpoint(
    const ControllerEndpointRequest& endpoint,
    std::string& error)
{
    const auto validation = ValidateControllerEndpoint(endpoint);
    if (validation.success)
        return true;
    error = "Invalid endpoint in the v5 recovery journal: "
        + validation.message;
    return false;
}

bool ValidateIdentity(
    const TunerRecoveryV5IdentityEvidence& identity,
    std::string& error)
{
    if (identity.mvlcHardwareId != ExpectedMvlcHardwareId
        || identity.mvlcFirmwareRevision
            != ExpectedMvlcFirmwareRevision)
    {
        error = "Invalid MVLC identity evidence in the v5 recovery journal.";
        return false;
    }
    if ((identity.targetBaseAddress & 0xFFFFU) != 0U
        || (identity.targetHardwareId != Mdpp32HardwareId
            && identity.targetHardwareId
                != Mdpp32AlternateHardwareId)
        || identity.targetFirmwareRevision
            != Mdpp32ScpFirmwareRevisionFw2051)
    {
        error = "Invalid target identity evidence in the v5 recovery journal.";
        return false;
    }
    return true;
}

bool ValidateOwnership(
    const TunerRecoveryV5OwnershipEvidence& ownership,
    const TunerRecoveryV5IdentityEvidence& identity,
    std::string& error)
{
    if (ownership.stackTriggerRegister == 0U
        || ownership.stackOffsetRegister == 0U
        || ownership.ownershipTokenRegister == 0U
        || ownership.ownershipTokenValue == 0U)
    {
        error = "Incomplete ownership evidence in the v5 recovery journal.";
        return false;
    }
    if (ownership.isolatedModuleBaseAddresses.size()
        > MaximumIsolatedModuleCount)
    {
        error = "Too many isolated modules in the v5 recovery journal.";
        return false;
    }
    for (std::size_t index = 0U;
         index < ownership.isolatedModuleBaseAddresses.size(); ++index)
    {
        const auto address = ownership.isolatedModuleBaseAddresses[index];
        if ((address & 0xFFFFU) != 0U
            || address == identity.targetBaseAddress)
        {
            error =
                "Invalid isolated-module evidence in the v5 recovery journal.";
            return false;
        }
        for (std::size_t previous = 0U; previous < index; ++previous)
        {
            if (ownership.isolatedModuleBaseAddresses[previous] == address)
            {
                error = "Invalid isolated-module evidence in the v5 recovery journal.";
                return false;
            }
        }
    }
    return true;
}

bool ValidateSnapshot(
    const Fw2051ScpConfigurationSnapshot& snapshot,
    const TunerRecoveryV5IdentityEvidence& identity,
    std::string& error)
{
    if (snapshot.state != ScpConfigurationState::Complete
        || !snapshot.selectorParkedAtQuadZero
        || snapshot.baseAddress != identity.targetBaseAddress
        || snapshot.hardwareId != identity.targetHardwareId
        || snapshot.firmwareRevision != identity.targetFirmwareRevision)
    {
        error = "Incomplete or inconsistent live restore snapshot in the v5 recovery journal.";
        return false;
    }
    if (snapshot.quads.size() != Fw2051ScpQuadCount)
    {
        error = "The v5 live restore snapshot must contain eight quads.";
        return false;
    }
    for (std::size_t index = 0U; index < snapshot.quads.size(); ++index)
    {
        if (snapshot.quads[index].quad != index)
        {
            error = "The v5 live restore snapshot quads are out of order.";
            return false;
        }
    }
    return true;
}

std::optional<std::uint16_t> SnapshotValue(
    const Fw2051ScpConfigurationSnapshot& snapshot,
    const TunerRecoveryV5Deviation& deviation)
{
    if (deviation.scope == TunerRecoveryDeviationScope::Global)
    {
        if (deviation.quad.has_value())
            return std::nullopt;
        if (deviation.registerOffset == IrqLevelRegister)
            return snapshot.irqLevel;
        if (deviation.registerOffset == OutputFormatRegister)
            return snapshot.outputFormat;
        return std::nullopt;
    }
    if (deviation.scope != TunerRecoveryDeviationScope::Quad
        || !deviation.quad.has_value()
        || *deviation.quad >= snapshot.quads.size()
        || FindFw2051ScpSetting(deviation.registerOffset) == nullptr)
    {
        return std::nullopt;
    }
    return Fw2051ScpQuadRegisterValue(
        snapshot.quads[*deviation.quad], deviation.registerOffset);
}

std::uint64_t DeviationLocationKey(
    const TunerRecoveryV5Deviation& deviation) noexcept
{
    const std::uint64_t scope = deviation.scope
            == TunerRecoveryDeviationScope::Quad
        ? 1ULL << 32U
        : 0U;
    const std::uint64_t quad = deviation.quad.has_value()
        ? static_cast<std::uint64_t>(*deviation.quad) << 16U
        : 0U;
    return scope | quad | deviation.registerOffset;
}

bool ValidateV5Data(
    const TunerRecoveryV5Data& data,
    std::string& error)
{
    if (SessionPhaseCode(data.sessionPhase) == 0U)
    {
        error = "Unknown session phase in the v5 recovery journal.";
        return false;
    }
    if (!ValidateEndpoint(data.endpoint, error)
        || !ValidateIdentity(data.identity, error))
    {
        return false;
    }
    if (data.ownership.has_value()
        && !ValidateOwnership(*data.ownership, data.identity, error))
    {
        return false;
    }
    if (!data.liveRestoreSnapshot.has_value())
    {
        if (data.sessionPhase != TuningSessionPhase::Preparing
            || !data.deviations.empty())
        {
            error = "Only an identity-only Preparing v5 record may omit the live restore snapshot.";
            return false;
        }
        return true;
    }
    if (!ValidateSnapshot(
            *data.liveRestoreSnapshot, data.identity, error))
    {
        return false;
    }
    if (data.deviations.size() > TunerRecoveryV5MaximumDeviationCount)
    {
        error = "Too many deviations in the v5 recovery journal.";
        return false;
    }

    std::unordered_map<std::uint64_t, std::uint16_t> workingValues;
    for (std::size_t index = 0U; index < data.deviations.size(); ++index)
    {
        const auto& deviation = data.deviations[index];
        if (deviation.ordinal != index
            || SessionPhaseCode(deviation.transitionPhase) == 0U)
        {
            error = "The v5 recovery deviations are out of order or carry an unknown phase.";
            return false;
        }
        const auto original = SnapshotValue(
            *data.liveRestoreSnapshot, deviation);
        if (!original.has_value()
            || *original != deviation.originalSessionValue)
        {
            error = "A v5 recovery deviation does not match the live restore snapshot.";
            return false;
        }

        const auto key = DeviationLocationKey(deviation);
        const auto previous = workingValues.find(key);
        const auto expectedPrevious = previous == workingValues.end()
            ? *original
            : previous->second;
        if (deviation.previousVerifiedWorkingValue != expectedPrevious
            || deviation.requestedNextValue == expectedPrevious)
        {
            error = "The v5 recovery deviation transition chain is inconsistent.";
            return false;
        }
        workingValues[key] = deviation.requestedNextValue;
    }
    return true;
}

bool LegacyEvidenceIsAbsent(const TunerRecoveryRecord& record)
{
    return record.host.empty()
        && record.mvlcHardwareId == 0U
        && record.mvlcFirmwareRevision == 0U
        && record.mdppBaseAddress == 0U
        && record.mdppHardwareId == 0U
        && record.mdppIrqLevel == 0U
        && record.mdppOutputFormat == 0U
        && record.stackTriggerRegister == 0U
        && record.stackTriggerValue == 0U
        && record.stackOffsetRegister == 0U
        && record.stackOffsetValue == 0U
        && record.ownershipTokenRegister == 0U
        && record.ownershipTokenValue == 0U
        && record.isolatedModuleBaseAddresses.empty()
        && !record.sourceRestoreRequired
        && record.sourceQuad == 0U
        && record.sourceOriginalConfiguration == 0U
        && !record.sourceAppliedConfigurationAvailable
        && record.sourceAppliedConfiguration == 0U
        && !record.previewRestoreRequired
        && record.previewQuad == 0U
        && record.previewRegisterOffset == 0U
        && record.previewOriginalValue == 0U
        && record.previewAppliedValue == 0U;
}

bool ValidateV5Record(
    const TunerRecoveryRecord& record,
    std::string& error)
{
    if (record.formatVersion != TunerRecoveryJournalV5FormatVersion
        || !record.version5.has_value())
    {
        error = "The v5 recovery journal lacks its v5 evidence model.";
        return false;
    }
    if (!LegacyEvidenceIsAbsent(record))
    {
        error = "A v5 recovery journal cannot mix legacy scalar evidence with v5 sections.";
        return false;
    }
    return ValidateV5Data(*record.version5, error);
}

std::uint64_t SessionChecksum(const TunerRecoveryV5Data& data)
{
    std::uint64_t hash = FnvOffsetBasis;
    HashText(hash, "SESSION");
    HashValue(hash, SessionPhaseCode(data.sessionPhase));
    HashValue(hash, data.selectorParkingRequired ? 1U : 0U);
    return hash;
}

std::uint64_t EndpointChecksum(const ControllerEndpointRequest& endpoint)
{
    std::uint64_t hash = FnvOffsetBasis;
    HashText(hash, "ENDPOINT");
    if (endpoint.kind == ControllerEndpointKind::DirectEthernet)
    {
        HashValue(hash, 1U);
        HashText(hash, endpoint.mvlcHost);
        HashValue(hash, endpoint.mvlcCommandPort);
    }
    else
    {
        HashValue(hash, 2U);
        HashText(hash, endpoint.mvlcHost);
        HashValue(hash, endpoint.mvlcCommandPort);
        HashText(hash, endpoint.sshDestination);
        HashText(hash, endpoint.remoteBridgeCommand);
    }
    return hash;
}

std::uint64_t IdentityChecksum(
    const TunerRecoveryV5IdentityEvidence& identity)
{
    std::uint64_t hash = FnvOffsetBasis;
    HashText(hash, "IDENTITY");
    HashValue(hash, identity.mvlcHardwareId);
    HashValue(hash, identity.mvlcFirmwareRevision);
    HashValue(hash, identity.targetBaseAddress);
    HashValue(hash, identity.targetHardwareId);
    HashValue(hash, identity.targetFirmwareRevision);
    return hash;
}

std::uint64_t OwnershipChecksum(
    const std::optional<TunerRecoveryV5OwnershipEvidence>& ownership)
{
    std::uint64_t hash = FnvOffsetBasis;
    HashText(hash, "OWNERSHIP");
    HashValue(hash, ownership.has_value() ? 1U : 0U);
    if (ownership.has_value())
    {
        HashValue(hash, ownership->stackTriggerRegister);
        HashValue(hash, ownership->stackTriggerValue);
        HashValue(hash, ownership->stackOffsetRegister);
        HashValue(hash, ownership->stackOffsetValue);
        HashValue(hash, ownership->ownershipTokenRegister);
        HashValue(hash, ownership->ownershipTokenValue);
        HashValue(
            hash,
            static_cast<std::uint32_t>(
                ownership->isolatedModuleBaseAddresses.size()));
        for (const auto address : ownership->isolatedModuleBaseAddresses)
            HashValue(hash, address);
    }
    return hash;
}

void HashQuad(
    std::uint64_t& hash,
    const Fw2051ScpQuadConfiguration& quad)
{
    HashValue(hash, quad.quad);
    HashValue(hash, quad.timingFilter);
    for (const auto value : quad.poleZero)
        HashValue(hash, value);
    HashValue(hash, quad.gain);
    for (const auto value : quad.thresholds)
        HashValue(hash, value);
    HashValue(hash, quad.shapingTime);
    HashValue(hash, quad.baselineRestorer);
    HashValue(hash, quad.resetTime);
    HashValue(hash, quad.signalRiseTime);
    HashValue(hash, quad.preSamples);
    HashValue(hash, quad.totalSamples);
    HashValue(hash, quad.sampleConfiguration);
}

std::uint64_t SnapshotChecksum(
    const std::optional<Fw2051ScpConfigurationSnapshot>& snapshot)
{
    std::uint64_t hash = FnvOffsetBasis;
    HashText(hash, "RESTORE_SNAPSHOT");
    HashValue(hash, snapshot.has_value() ? 1U : 0U);
    if (snapshot.has_value())
    {
        HashValue(hash, snapshot->baseAddress);
        HashValue(hash, snapshot->hardwareId);
        HashValue(hash, snapshot->firmwareRevision);
        HashValue(hash, snapshot->irqLevel);
        HashValue(hash, snapshot->outputFormat);
        HashValue(hash, static_cast<std::uint32_t>(snapshot->quads.size()));
        for (const auto& quad : snapshot->quads)
            HashQuad(hash, quad);
    }
    return hash;
}

std::uint64_t DeviationsChecksum(
    const std::vector<TunerRecoveryV5Deviation>& deviations)
{
    std::uint64_t hash = FnvOffsetBasis;
    HashText(hash, "DEVIATIONS");
    HashValue(hash, static_cast<std::uint32_t>(deviations.size()));
    for (const auto& deviation : deviations)
    {
        HashValue(hash, deviation.ordinal);
        HashValue(hash, static_cast<std::uint16_t>(deviation.scope));
        HashValue(hash, deviation.quad.has_value() ? 1U : 0U);
        HashValue(hash, deviation.quad.value_or(0U));
        HashValue(hash, deviation.registerOffset);
        HashValue(hash, deviation.originalSessionValue);
        HashValue(hash, deviation.previousVerifiedWorkingValue);
        HashValue(hash, deviation.requestedNextValue);
        HashValue(hash, SessionPhaseCode(deviation.transitionPhase));
    }
    return hash;
}

std::uint64_t RecordChecksum(const TunerRecoveryV5Data& data)
{
    std::uint64_t hash = FnvOffsetBasis;
    HashText(hash, "MWW_TUNER_RECOVERY_V5");
    HashValue(hash, TunerRecoveryJournalV5FormatVersion);
    HashWideValue(hash, SessionChecksum(data));
    HashWideValue(hash, EndpointChecksum(data.endpoint));
    HashWideValue(hash, IdentityChecksum(data.identity));
    HashWideValue(hash, OwnershipChecksum(data.ownership));
    HashWideValue(hash, SnapshotChecksum(data.liveRestoreSnapshot));
    HashWideValue(hash, DeviationsChecksum(data.deviations));
    return hash;
}

void WriteQuad(
    std::ostream& output,
    const Fw2051ScpQuadConfiguration& quad)
{
    output << "QUAD " << quad.quad << ' ' << quad.timingFilter;
    for (const auto value : quad.poleZero)
        output << ' ' << value;
    output << ' ' << quad.gain;
    for (const auto value : quad.thresholds)
        output << ' ' << value;
    output << ' ' << quad.shapingTime
           << ' ' << quad.baselineRestorer
           << ' ' << quad.resetTime
           << ' ' << quad.signalRiseTime
           << ' ' << quad.preSamples
           << ' ' << quad.totalSamples
           << ' ' << quad.sampleConfiguration << '\n';
}

void WriteV5(std::ostream& output, const TunerRecoveryV5Data& data)
{
    output << JournalMagic << ' '
           << TunerRecoveryJournalV5FormatVersion << '\n';
    output << "SESSION " << SessionPhaseToken(data.sessionPhase) << ' '
           << (data.selectorParkingRequired ? 1U : 0U) << ' '
           << SessionChecksum(data) << '\n';

    if (data.endpoint.kind == ControllerEndpointKind::DirectEthernet)
    {
        output << "ENDPOINT DIRECT " << data.endpoint.mvlcHost << ' '
               << data.endpoint.mvlcCommandPort << ' '
               << EndpointChecksum(data.endpoint) << '\n';
    }
    else
    {
        output << "ENDPOINT SSH_BRIDGE " << data.endpoint.mvlcHost << ' '
               << data.endpoint.mvlcCommandPort << ' '
               << data.endpoint.sshDestination << ' '
               << data.endpoint.remoteBridgeCommand << ' '
               << EndpointChecksum(data.endpoint) << '\n';
    }

    output << "IDENTITY " << data.identity.mvlcHardwareId << ' '
           << data.identity.mvlcFirmwareRevision << ' '
           << data.identity.targetBaseAddress << ' '
           << data.identity.targetHardwareId << ' '
           << data.identity.targetFirmwareRevision << ' '
           << IdentityChecksum(data.identity) << '\n';

    output << "OWNERSHIP " << (data.ownership.has_value() ? 1U : 0U);
    if (data.ownership.has_value())
    {
        const auto& ownership = *data.ownership;
        output << ' ' << ownership.stackTriggerRegister
               << ' ' << ownership.stackTriggerValue
               << ' ' << ownership.stackOffsetRegister
               << ' ' << ownership.stackOffsetValue
               << ' ' << ownership.ownershipTokenRegister
               << ' ' << ownership.ownershipTokenValue
               << ' ' << ownership.isolatedModuleBaseAddresses.size();
        for (const auto address : ownership.isolatedModuleBaseAddresses)
            output << ' ' << address;
    }
    output << '\n' << "OWNERSHIP_CHECKSUM "
           << OwnershipChecksum(data.ownership) << '\n';

    output << "RESTORE_SNAPSHOT "
           << (data.liveRestoreSnapshot.has_value() ? 1U : 0U) << '\n';
    if (data.liveRestoreSnapshot.has_value())
    {
        const auto& snapshot = *data.liveRestoreSnapshot;
        output << "GLOBALS " << snapshot.baseAddress << ' '
               << snapshot.hardwareId << ' '
               << snapshot.firmwareRevision << ' '
               << snapshot.irqLevel << ' '
               << snapshot.outputFormat << '\n';
        for (const auto& quad : snapshot.quads)
            WriteQuad(output, quad);
    }
    output << "SNAPSHOT_CHECKSUM "
           << SnapshotChecksum(data.liveRestoreSnapshot) << '\n';

    output << "DEVIATIONS " << data.deviations.size() << '\n';
    for (const auto& deviation : data.deviations)
    {
        output << "DEVIATION " << deviation.ordinal << ' ';
        if (deviation.scope == TunerRecoveryDeviationScope::Global)
        {
            output << "GLOBAL ";
        }
        else
        {
            output << "QUAD " << deviation.quad.value_or(0U) << ' ';
        }
        output << deviation.registerOffset << ' '
               << deviation.originalSessionValue << ' '
               << deviation.previousVerifiedWorkingValue << ' '
               << deviation.requestedNextValue << ' '
               << SessionPhaseToken(deviation.transitionPhase) << '\n';
    }
    output << "DEVIATIONS_CHECKSUM "
           << DeviationsChecksum(data.deviations) << '\n';
    output << "RECORD_CHECKSUM " << RecordChecksum(data) << '\n';
    output << "END\n";
}

bool ExpectToken(
    std::istream& input,
    const std::string_view expected,
    std::string& error)
{
    std::string token;
    if (!(input >> token))
    {
        error = "Unexpected end of v5 recovery journal while reading '"
            + std::string(expected) + "'.";
        return false;
    }
    if (token != expected)
    {
        error = "Expected v5 recovery token '" + std::string(expected)
            + "', found '" + token + "'.";
        return false;
    }
    return true;
}

template<typename Integer>
bool ReadUnsigned(
    std::istream& input,
    Integer& destination,
    const char* field,
    std::string& error)
{
    static_assert(std::is_unsigned_v<Integer>);
    std::string token;
    if (!(input >> token))
    {
        error = std::string("Missing unsigned value for v5 ") + field + '.';
        return false;
    }
    std::uint64_t parsed = 0U;
    const auto conversion = std::from_chars(
        token.data(), token.data() + token.size(), parsed, 10);
    if (conversion.ec != std::errc{}
        || conversion.ptr != token.data() + token.size()
        || parsed > std::numeric_limits<Integer>::max())
    {
        error = std::string("Invalid unsigned value '") + token
            + "' for v5 " + field + '.';
        return false;
    }
    destination = static_cast<Integer>(parsed);
    return true;
}

bool CheckSectionChecksum(
    const std::uint64_t stored,
    const std::uint64_t expected,
    const char* section,
    std::string& error)
{
    if (stored == expected)
        return true;
    error = std::string("V5 recovery-journal ") + section
        + " checksum mismatch.";
    return false;
}

bool ReadQuad(
    std::istream& input,
    Fw2051ScpQuadConfiguration& quad,
    std::string& error)
{
    if (!ExpectToken(input, "QUAD", error)
        || !ReadUnsigned(input, quad.quad, "quad", error)
        || !ReadUnsigned(input, quad.timingFilter, "timing filter", error))
    {
        return false;
    }
    for (auto& value : quad.poleZero)
    {
        if (!ReadUnsigned(input, value, "pole-zero", error))
            return false;
    }
    if (!ReadUnsigned(input, quad.gain, "gain", error))
        return false;
    for (auto& value : quad.thresholds)
    {
        if (!ReadUnsigned(input, value, "threshold", error))
            return false;
    }
    return ReadUnsigned(input, quad.shapingTime, "shaping time", error)
        && ReadUnsigned(
            input, quad.baselineRestorer, "baseline restorer", error)
        && ReadUnsigned(input, quad.resetTime, "reset time", error)
        && ReadUnsigned(
            input, quad.signalRiseTime, "signal rise time", error)
        && ReadUnsigned(input, quad.preSamples, "pre-samples", error)
        && ReadUnsigned(input, quad.totalSamples, "total samples", error)
        && ReadUnsigned(
            input,
            quad.sampleConfiguration,
            "sample configuration",
            error);
}

bool ReadEndpoint(
    std::istream& input,
    ControllerEndpointRequest& endpoint,
    std::uint64_t& checksum,
    std::string& error)
{
    std::string kind;
    std::string host;
    std::uint16_t port = 0U;
    if (!ExpectToken(input, "ENDPOINT", error)
        || !(input >> kind >> host)
        || !ReadUnsigned(input, port, "endpoint port", error))
    {
        if (error.empty())
            error = "Malformed endpoint section in the v5 recovery journal.";
        return false;
    }
    if (kind == "DIRECT")
    {
        endpoint = {
            ControllerEndpointKind::DirectEthernet,
            std::move(host),
            port,
            {},
            {},
        };
    }
    else if (kind == "SSH_BRIDGE")
    {
        std::string destination;
        std::string command;
        if (!(input >> destination >> command))
        {
            error = "Incomplete SSH endpoint in the v5 recovery journal.";
            return false;
        }
        endpoint = {
            ControllerEndpointKind::SshBridge,
            std::move(host),
            port,
            std::move(destination),
            std::move(command),
        };
    }
    else
    {
        error = "Unknown endpoint kind in the v5 recovery journal.";
        return false;
    }
    return ReadUnsigned(input, checksum, "endpoint checksum", error);
}

bool ReadOwnership(
    std::istream& input,
    std::optional<TunerRecoveryV5OwnershipEvidence>& ownership,
    std::uint64_t& checksum,
    std::string& error)
{
    std::uint16_t present = 0U;
    if (!ExpectToken(input, "OWNERSHIP", error)
        || !ReadUnsigned(input, present, "ownership presence", error)
        || present > 1U)
    {
        if (error.empty())
            error = "Invalid ownership presence in the v5 recovery journal.";
        return false;
    }
    if (present == 1U)
    {
        TunerRecoveryV5OwnershipEvidence value;
        std::uint16_t isolatedCount = 0U;
        if (!ReadUnsigned(
                input, value.stackTriggerRegister,
                "stack trigger register", error)
            || !ReadUnsigned(
                input, value.stackTriggerValue,
                "stack trigger value", error)
            || !ReadUnsigned(
                input, value.stackOffsetRegister,
                "stack offset register", error)
            || !ReadUnsigned(
                input, value.stackOffsetValue,
                "stack offset value", error)
            || !ReadUnsigned(
                input, value.ownershipTokenRegister,
                "ownership token register", error)
            || !ReadUnsigned(
                input, value.ownershipTokenValue,
                "ownership token value", error)
            || !ReadUnsigned(
                input, isolatedCount, "isolated module count", error))
        {
            return false;
        }
        if (isolatedCount > MaximumIsolatedModuleCount)
        {
            error = "Too many isolated modules in the v5 recovery journal.";
            return false;
        }
        value.isolatedModuleBaseAddresses.reserve(isolatedCount);
        for (std::uint16_t index = 0U; index < isolatedCount; ++index)
        {
            std::uint32_t address = 0U;
            if (!ReadUnsigned(
                    input, address, "isolated module address", error))
            {
                return false;
            }
            value.isolatedModuleBaseAddresses.push_back(address);
        }
        ownership = std::move(value);
    }
    if (!ExpectToken(input, "OWNERSHIP_CHECKSUM", error)
        || !ReadUnsigned(input, checksum, "ownership checksum", error))
    {
        return false;
    }
    return true;
}

bool ReadSnapshot(
    std::istream& input,
    std::optional<Fw2051ScpConfigurationSnapshot>& snapshot,
    std::uint64_t& checksum,
    std::string& error)
{
    std::uint16_t present = 0U;
    if (!ExpectToken(input, "RESTORE_SNAPSHOT", error)
        || !ReadUnsigned(input, present, "snapshot presence", error)
        || present > 1U)
    {
        if (error.empty())
            error = "Invalid snapshot presence in the v5 recovery journal.";
        return false;
    }
    if (present == 1U)
    {
        Fw2051ScpConfigurationSnapshot value;
        value.state = ScpConfigurationState::Complete;
        value.message = "Parsed from v5 recovery journal.";
        value.selectorParkedAtQuadZero = true;
        if (!ExpectToken(input, "GLOBALS", error)
            || !ReadUnsigned(input, value.baseAddress, "target base", error)
            || !ReadUnsigned(input, value.hardwareId, "target hardware", error)
            || !ReadUnsigned(
                input, value.firmwareRevision, "target firmware", error)
            || !ReadUnsigned(input, value.irqLevel, "IRQ level", error)
            || !ReadUnsigned(input, value.outputFormat, "output format", error))
        {
            return false;
        }
        value.quads.reserve(Fw2051ScpQuadCount);
        for (std::size_t index = 0U; index < Fw2051ScpQuadCount; ++index)
        {
            Fw2051ScpQuadConfiguration quad;
            if (!ReadQuad(input, quad, error))
                return false;
            value.quads.push_back(std::move(quad));
        }
        snapshot = std::move(value);
    }
    if (!ExpectToken(input, "SNAPSHOT_CHECKSUM", error)
        || !ReadUnsigned(input, checksum, "snapshot checksum", error))
    {
        return false;
    }
    return true;
}

bool ReadDeviation(
    std::istream& input,
    TunerRecoveryV5Deviation& deviation,
    std::string& error)
{
    std::string scope;
    if (!ExpectToken(input, "DEVIATION", error)
        || !ReadUnsigned(input, deviation.ordinal, "deviation ordinal", error)
        || !(input >> scope))
    {
        if (error.empty())
            error = "Malformed deviation in the v5 recovery journal.";
        return false;
    }
    if (scope == "GLOBAL")
    {
        deviation.scope = TunerRecoveryDeviationScope::Global;
        deviation.quad.reset();
    }
    else if (scope == "QUAD")
    {
        deviation.scope = TunerRecoveryDeviationScope::Quad;
        std::uint16_t quad = 0U;
        if (!ReadUnsigned(input, quad, "deviation quad", error))
            return false;
        deviation.quad = quad;
    }
    else
    {
        error = "Unknown deviation scope in the v5 recovery journal.";
        return false;
    }

    std::string phase;
    if (!ReadUnsigned(
            input, deviation.registerOffset,
            "deviation register", error)
        || !ReadUnsigned(
            input, deviation.originalSessionValue,
            "deviation original value", error)
        || !ReadUnsigned(
            input, deviation.previousVerifiedWorkingValue,
            "deviation previous value", error)
        || !ReadUnsigned(
            input, deviation.requestedNextValue,
            "deviation requested value", error)
        || !(input >> phase))
    {
        if (error.empty())
            error = "Incomplete deviation in the v5 recovery journal.";
        return false;
    }
    if (!ParseSessionPhase(phase, deviation.transitionPhase))
    {
        error = "Unknown deviation phase in the v5 recovery journal.";
        return false;
    }
    return true;
}

} // namespace

TunerRecoverySerializationResult SerializeTunerRecoveryJournalV5(
    const TunerRecoveryRecord& record)
{
    TunerRecoverySerializationResult result;
    std::string error;
    if (!ValidateV5Record(record, error))
    {
        result.message = error;
        return result;
    }

    std::ostringstream output;
    WriteV5(output, *record.version5);
    if (!output)
    {
        result.message = "Writing the v5 recovery journal failed.";
        return result;
    }
    result.success = true;
    result.message = "Serialized v5 tuner recovery journal.";
    result.text = output.str();
    return result;
}

TunerRecoveryParseResult ParseTunerRecoveryJournalV5(
    const std::string& text)
{
    TunerRecoveryParseResult result;
    std::istringstream input(text);
    std::string error;
    std::string magic;
    std::uint32_t version = 0U;
    TunerRecoveryV5Data data;
    std::string phase;
    std::uint16_t selectorParkingRequired = 0U;
    std::uint64_t sessionChecksum = 0U;
    std::uint64_t endpointChecksum = 0U;
    std::uint64_t identityChecksum = 0U;
    std::uint64_t ownershipChecksum = 0U;
    std::uint64_t snapshotChecksum = 0U;
    std::uint64_t deviationsChecksum = 0U;
    std::uint64_t recordChecksum = 0U;

    if (!(input >> magic) || magic != JournalMagic
        || !ReadUnsigned(input, version, "format version", error)
        || version != TunerRecoveryJournalV5FormatVersion)
    {
        result.message = error.empty()
            ? "Invalid v5 tuner recovery-journal header."
            : error;
        return result;
    }
    if (!ExpectToken(input, "SESSION", error)
        || !(input >> phase)
        || !ReadUnsigned(
            input, selectorParkingRequired,
            "selector parking state", error)
        || selectorParkingRequired > 1U
        || !ReadUnsigned(input, sessionChecksum, "session checksum", error))
    {
        result.message = error.empty()
            ? "Malformed session section in the v5 recovery journal."
            : error;
        return result;
    }
    if (!ParseSessionPhase(phase, data.sessionPhase))
    {
        result.message = "Unknown session phase in the v5 recovery journal.";
        return result;
    }
    data.selectorParkingRequired = selectorParkingRequired == 1U;
    if (!CheckSectionChecksum(
            sessionChecksum, SessionChecksum(data), "session", error)
        || !ReadEndpoint(
            input, data.endpoint, endpointChecksum, error)
        || !CheckSectionChecksum(
            endpointChecksum,
            EndpointChecksum(data.endpoint),
            "endpoint",
            error))
    {
        result.message = error;
        return result;
    }

    if (!ExpectToken(input, "IDENTITY", error)
        || !ReadUnsigned(
            input, data.identity.mvlcHardwareId,
            "MVLC hardware ID", error)
        || !ReadUnsigned(
            input, data.identity.mvlcFirmwareRevision,
            "MVLC firmware", error)
        || !ReadUnsigned(
            input, data.identity.targetBaseAddress,
            "target base", error)
        || !ReadUnsigned(
            input, data.identity.targetHardwareId,
            "target hardware ID", error)
        || !ReadUnsigned(
            input, data.identity.targetFirmwareRevision,
            "target firmware", error)
        || !ReadUnsigned(
            input, identityChecksum, "identity checksum", error)
        || !CheckSectionChecksum(
            identityChecksum,
            IdentityChecksum(data.identity),
            "identity",
            error)
        || !ReadOwnership(
            input, data.ownership, ownershipChecksum, error)
        || !CheckSectionChecksum(
            ownershipChecksum,
            OwnershipChecksum(data.ownership),
            "ownership",
            error)
        || !ReadSnapshot(
            input, data.liveRestoreSnapshot, snapshotChecksum, error)
        || !CheckSectionChecksum(
            snapshotChecksum,
            SnapshotChecksum(data.liveRestoreSnapshot),
            "snapshot",
            error))
    {
        result.message = error;
        return result;
    }

    std::uint32_t deviationCount = 0U;
    if (!ExpectToken(input, "DEVIATIONS", error)
        || !ReadUnsigned(
            input, deviationCount, "deviation count", error))
    {
        result.message = error;
        return result;
    }
    if (deviationCount > TunerRecoveryV5MaximumDeviationCount)
    {
        result.message = "Too many deviations in the v5 recovery journal.";
        return result;
    }
    data.deviations.reserve(deviationCount);
    for (std::uint32_t index = 0U; index < deviationCount; ++index)
    {
        TunerRecoveryV5Deviation deviation;
        if (!ReadDeviation(input, deviation, error))
        {
            result.message = error;
            return result;
        }
        if (deviation.ordinal != index)
        {
            result.message =
                "The v5 recovery deviations are out of order.";
            return result;
        }
        data.deviations.push_back(std::move(deviation));
    }
    if (!ExpectToken(input, "DEVIATIONS_CHECKSUM", error)
        || !ReadUnsigned(
            input, deviationsChecksum,
            "deviations checksum", error)
        || !CheckSectionChecksum(
            deviationsChecksum,
            DeviationsChecksum(data.deviations),
            "deviations",
            error)
        || !ExpectToken(input, "RECORD_CHECKSUM", error)
        || !ReadUnsigned(input, recordChecksum, "record checksum", error)
        || !CheckSectionChecksum(
            recordChecksum, RecordChecksum(data), "record", error)
        || !ExpectToken(input, "END", error))
    {
        result.message = error;
        return result;
    }
    std::string trailing;
    if (input >> trailing)
    {
        result.message = "Unexpected trailing data in v5 recovery journal.";
        return result;
    }

    TunerRecoveryRecord record;
    record.formatVersion = TunerRecoveryJournalV5FormatVersion;
    record.version5 = std::move(data);
    if (!ValidateV5Record(record, error))
    {
        result.message = error;
        return result;
    }
    result.success = true;
    result.message = "Parsed v5 tuner recovery journal.";
    result.record = std::move(record);
    return result;
}

std::array<TunerRecoveryEvidenceDefinedValue, 3U>
TunerRecoveryV5EvidenceDefinedValues(
    const TunerRecoveryV5Deviation& deviation) noexcept
{
    return {{
        {
            TunerRecoveryLiveValueEvidence::OriginalSession,
            deviation.originalSessionValue,
            TunerRecoveryLiveValueAction::NoWriteAlreadyOriginal,
        },
        {
            TunerRecoveryLiveValueEvidence::PreviousVerifiedWorking,
            deviation.previousVerifiedWorkingValue,
            TunerRecoveryLiveValueAction::RestoreOriginal,
        },
        {
            TunerRecoveryLiveValueEvidence::RequestedNext,
            deviation.requestedNextValue,
            TunerRecoveryLiveValueAction::RestoreOriginal,
        },
    }};
}

TunerRecoveryLiveValueDecision ClassifyTunerRecoveryV5LiveValue(
    const TunerRecoveryV5Deviation& deviation,
    const std::uint16_t liveValue) noexcept
{
    for (const auto& accepted :
         TunerRecoveryV5EvidenceDefinedValues(deviation))
    {
        if (liveValue == accepted.value)
            return {accepted.evidence, accepted.action};
    }
    return {};
}

} // namespace fidget
