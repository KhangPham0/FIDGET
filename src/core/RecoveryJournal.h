#ifndef FIDGET_CORE_RECOVERY_JOURNAL_H
#define FIDGET_CORE_RECOVERY_JOURNAL_H

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fidget {

inline constexpr std::uint32_t TunerRecoveryJournalFormatVersion = 4U;

enum class TunerRecoveryPhase : std::uint16_t
{
    Prepared = 1U,
    Active = 2U,
};

struct TunerRecoveryRecord
{
    std::uint32_t formatVersion = TunerRecoveryJournalFormatVersion;
    TunerRecoveryPhase phase = TunerRecoveryPhase::Prepared;
    std::string host;
    std::uint16_t commandPort = 32768U;
    std::uint32_t mvlcHardwareId = 0U;
    std::uint32_t mvlcFirmwareRevision = 0U;
    std::uint32_t mdppBaseAddress = 0U;
    std::uint16_t mdppHardwareId = 0U;
    std::uint16_t mdppIrqLevel = 0U;
    std::uint16_t mdppOutputFormat = 0U;
    std::uint16_t stackTriggerRegister = 0U;
    std::uint32_t stackTriggerValue = 0U;
    std::uint16_t stackOffsetRegister = 0U;
    std::uint32_t stackOffsetValue = 0U;
    std::uint16_t ownershipTokenRegister = 0U;
    std::uint32_t ownershipTokenValue = 0U;
    std::vector<std::uint32_t> isolatedModuleBaseAddresses;
    bool sourceRestoreRequired = false;
    std::uint16_t sourceQuad = 0U;
    std::uint16_t sourceOriginalConfiguration = 0U;
    bool sourceAppliedConfigurationAvailable = false;
    std::uint16_t sourceAppliedConfiguration = 0U;
    bool previewRestoreRequired = false;
    std::uint16_t previewQuad = 0U;
    std::uint16_t previewRegisterOffset = 0U;
    std::uint16_t previewOriginalValue = 0U;
    std::uint16_t previewAppliedValue = 0U;
};

struct TunerRecoverySerializationResult
{
    bool success = false;
    std::string message;
    std::string text;
};

struct TunerRecoveryParseResult
{
    bool success = false;
    std::string message;
    std::optional<TunerRecoveryRecord> record;
};

struct TunerRecoverySaveResult
{
    bool success = false;
    std::string message;
};

struct TunerRecoveryLoadResult
{
    bool success = false;
    bool fileMissing = false;
    std::string message;
    std::optional<TunerRecoveryRecord> record;
};

[[nodiscard]] TunerRecoverySerializationResult SerializeTunerRecoveryJournal(
    const TunerRecoveryRecord& record);

[[nodiscard]] TunerRecoveryParseResult ParseTunerRecoveryJournal(
    const std::string& text);

[[nodiscard]] TunerRecoverySaveResult SaveTunerRecoveryJournal(
    const TunerRecoveryRecord& record,
    const std::string& path);

[[nodiscard]] TunerRecoveryLoadResult LoadTunerRecoveryJournal(
    const std::string& path);

[[nodiscard]] bool RemoveTunerRecoveryJournal(
    const std::string& path,
    std::string& error);

[[nodiscard]] std::string ProjectTunerRecoveryJournalPath(
    const std::string& projectPath);

} // namespace fidget

#endif
