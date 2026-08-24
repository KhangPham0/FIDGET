#ifndef FIDGET_CORE_APPLICATION_RECOVERY_DISCOVERY_H
#define FIDGET_CORE_APPLICATION_RECOVERY_DISCOVERY_H

#include "core/ApplicationStorage.h"
#include "core/RecoveryJournal.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fidget {

enum class ApplicationRecoveryDiscoveryState
{
    NotScanned,
    Empty,
    PendingV5,
    Blocked,
};

enum class ApplicationRecoveryBlockReason
{
    None,
    StorageUnavailable,
    MultipleRecords,
    MalformedRecord,
    UnsupportedLegacyVersion,
};

// Files in the application recovery directory are discovery evidence only.
// Discovery never creates the directory and never deletes, rewrites, renames,
// or otherwise repairs an entry. A PendingV5 result carries the complete
// parsed record so later recovery needs neither a project nor a profile file.
struct ApplicationRecoveryDiscoveryResult
{
    ApplicationRecoveryDiscoveryState state =
        ApplicationRecoveryDiscoveryState::NotScanned;
    ApplicationRecoveryBlockReason blockReason =
        ApplicationRecoveryBlockReason::None;
    std::string message;
    std::vector<std::filesystem::path> recordPaths;
    std::optional<TunerRecoveryRecord> record;
};

[[nodiscard]] ApplicationRecoveryDiscoveryResult
DiscoverApplicationRecovery(
    const ApplicationStoragePaths& paths);

[[nodiscard]] bool ApplicationRecoveryBlocksNormalTuning(
    const ApplicationRecoveryDiscoveryResult& discovery) noexcept;

[[nodiscard]] bool ApplicationRecoveryHasRetainedEvidence(
    const ApplicationRecoveryDiscoveryResult& discovery) noexcept;

} // namespace fidget

#endif
