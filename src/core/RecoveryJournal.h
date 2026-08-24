#ifndef FIDGET_CORE_RECOVERY_JOURNAL_H
#define FIDGET_CORE_RECOVERY_JOURNAL_H

#include "core/ControllerEndpoint.h"
#include "core/ScpConfiguration.h"
#include "core/TuningSessionState.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fidget {

// Existing hardware operations deliberately continue to construct v4 until
// the v5 session writer arrives. The parser accepts the separately named v5
// format without silently changing any current writer's default.
inline constexpr std::uint32_t TunerRecoveryJournalFormatVersion = 4U;
inline constexpr std::uint32_t TunerRecoveryJournalV5FormatVersion = 5U;
inline constexpr std::size_t TunerRecoveryV5MaximumDeviationCount = 1024U;
// Every atomic-save workspace is a direct child of the recovery directory
// whose name begins with this reserved prefix. It never has a journal suffix
// and must be ignored by application-directory recovery discovery when the
// phase-D discovery implementation is rebased.
inline constexpr std::string_view TunerRecoveryJournalStagingDirectoryPrefix =
    ".fidget-journal-staging";

enum class TunerRecoveryPhase : std::uint16_t
{
    Prepared = 1U,
    Active = 2U,
};

struct TunerRecoveryV5IdentityEvidence
{
    std::uint32_t mvlcHardwareId = 0U;
    std::uint32_t mvlcFirmwareRevision = 0U;
    // The one normalized full A32 value. Address zero is valid and is not an
    // unset sentinel.
    std::uint32_t targetBaseAddress = 0U;
    std::uint16_t targetHardwareId = 0U;
    std::uint16_t targetFirmwareRevision = 0U;
};

// This fingerprint is absent in the identity-only Prepared record and becomes
// available only after a later acquisition operation establishes ownership.
struct TunerRecoveryV5OwnershipEvidence
{
    std::uint16_t stackTriggerRegister = 0U;
    std::uint32_t stackTriggerValue = 0U;
    std::uint16_t stackOffsetRegister = 0U;
    std::uint32_t stackOffsetValue = 0U;
    std::uint16_t ownershipTokenRegister = 0U;
    std::uint32_t ownershipTokenValue = 0U;
    std::vector<std::uint32_t> isolatedModuleBaseAddresses;
};

enum class TunerRecoveryDeviationScope : std::uint16_t
{
    Global = 1U,
    Quad = 2U,
};

struct TunerRecoveryV5Deviation
{
    // The serialized ordinal must equal the entry's vector position. It makes
    // truncation, reordering, and duplicate insertion detectable before any
    // recovery operation can trust the transition chain.
    std::uint32_t ordinal = 0U;
    TunerRecoveryDeviationScope scope =
        TunerRecoveryDeviationScope::Global;
    std::optional<std::uint16_t> quad;
    std::uint16_t registerOffset = 0U;
    std::uint16_t originalSessionValue = 0U;
    std::uint16_t previousVerifiedWorkingValue = 0U;
    std::uint16_t requestedNextValue = 0U;
    TuningSessionPhase transitionPhase = TuningSessionPhase::Preparing;
};

enum class TunerRecoveryLiveValueEvidence : std::uint16_t
{
    None = 0U,
    OriginalSession = 1U,
    PreviousVerifiedWorking = 2U,
    RequestedNext = 3U,
};

enum class TunerRecoveryLiveValueAction : std::uint16_t
{
    BlockAndRetain = 0U,
    NoWriteAlreadyOriginal = 1U,
    RestoreOriginal = 2U,
};

struct TunerRecoveryEvidenceDefinedValue
{
    TunerRecoveryLiveValueEvidence evidence =
        TunerRecoveryLiveValueEvidence::None;
    std::uint16_t value = 0U;
    TunerRecoveryLiveValueAction action =
        TunerRecoveryLiveValueAction::BlockAndRetain;
};

struct TunerRecoveryLiveValueDecision
{
    TunerRecoveryLiveValueEvidence evidence =
        TunerRecoveryLiveValueEvidence::None;
    TunerRecoveryLiveValueAction action =
        TunerRecoveryLiveValueAction::BlockAndRetain;
};

struct TunerRecoveryV5Data
{
    TuningSessionPhase sessionPhase = TuningSessionPhase::Preparing;
    ControllerEndpointRequest endpoint;
    TunerRecoveryV5IdentityEvidence identity;

    // A Prepared record may be installed before the first selector write. In
    // that state the snapshot is absent and selectorParkingRequired records
    // the only recovery action that capture may have made necessary. Once
    // present, the snapshot is complete: all five globals and all 17 values
    // for each of eight quads are checksum protected.
    bool selectorParkingRequired = false;
    std::optional<TunerRecoveryV5OwnershipEvidence> ownership;
    std::optional<Fw2051ScpConfigurationSnapshot> liveRestoreSnapshot;
    std::vector<TunerRecoveryV5Deviation> deviations;
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

    // V5 is deliberately disjoint from the legacy scalar fields above. A v5
    // writer must opt in by setting formatVersion to 5 and supplying this
    // complete extension; existing v1-v4 operations therefore keep emitting
    // byte-identical records until the session writer is implemented.
    std::optional<TunerRecoveryV5Data> version5;
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
    // Installation can have completed even when the following directory
    // durability sync fails. Callers must treat this as retained recovery
    // evidence while refusing any hardware action that required save success.
    bool destinationInstalled = false;
};

using TunerRecoveryJournalWriter =
    std::function<bool(std::ostream&, std::string_view)>;
using TunerRecoveryJournalSynchronizer =
    std::function<bool(const std::string&, std::string&)>;
using TunerRecoveryJournalReplacer = std::function<bool(
    const std::string&, const std::string&, std::string&)>;
using TunerRecoveryJournalFilesystemVerifier = std::function<bool(
    const std::string&, const std::string&, std::string&)>;

// Optional deterministic fault-injection seams. Production callers use the
// defaults: complete write, an explicit same-filesystem check, fsync of the
// staging file and destination directory, and one atomic rename with no delete
// fallback.
struct TunerRecoveryJournalSaveRuntime
{
    TunerRecoveryJournalWriter writer;
    TunerRecoveryJournalSynchronizer synchronize;
    TunerRecoveryJournalReplacer replace;
    TunerRecoveryJournalFilesystemVerifier sameFilesystem;
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

// Recovery accepts only these three named pieces of journal evidence. The
// original value requires no write; either verified/requested working value
// permits only restoration to the original. An unlisted value always returns
// BlockAndRetain. The recovery operation that enforces this decision arrives
// after the v5 format work.
[[nodiscard]] std::array<TunerRecoveryEvidenceDefinedValue, 3U>
TunerRecoveryV5EvidenceDefinedValues(
    const TunerRecoveryV5Deviation& deviation) noexcept;

[[nodiscard]] TunerRecoveryLiveValueDecision
ClassifyTunerRecoveryV5LiveValue(
    const TunerRecoveryV5Deviation& deviation,
    std::uint16_t liveValue) noexcept;

// The destination parent must already be a durably prepared plain directory.
// Application-storage callers establish that invariant with
// EnsureApplicationStorageDirectories(); legacy project-adjacent callers use
// the already-existing project directory.
[[nodiscard]] TunerRecoverySaveResult SaveTunerRecoveryJournal(
    const TunerRecoveryRecord& record,
    const std::string& path,
    const TunerRecoveryJournalSaveRuntime& runtime = {});

[[nodiscard]] TunerRecoveryLoadResult LoadTunerRecoveryJournal(
    const std::string& path);

[[nodiscard]] bool RemoveTunerRecoveryJournal(
    const std::string& path,
    std::string& error);

[[nodiscard]] std::string ProjectTunerRecoveryJournalPath(
    const std::string& projectPath);

} // namespace fidget

#endif
