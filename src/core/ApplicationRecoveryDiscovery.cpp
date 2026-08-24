#include "core/ApplicationRecoveryDiscovery.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <sstream>
#include <system_error>
#include <utility>

namespace fidget {
namespace {

ApplicationRecoveryDiscoveryResult Blocked(
    const ApplicationRecoveryBlockReason reason,
    std::string message,
    std::vector<std::filesystem::path> recordPaths = {})
{
    ApplicationRecoveryDiscoveryResult result;
    result.state = ApplicationRecoveryDiscoveryState::Blocked;
    result.blockReason = reason;
    result.message = std::move(message);
    result.recordPaths = std::move(recordPaths);
    return result;
}

std::string EntryNames(
    const std::vector<std::filesystem::path>& paths)
{
    std::ostringstream names;
    for (std::size_t index = 0U; index < paths.size(); ++index)
    {
        if (index != 0U)
            names << ", ";
        names << paths[index].filename().string();
    }
    return names.str();
}

bool IsJournalStagingEntry(const std::filesystem::path& path)
{
    const auto name = path.filename().string();
    const std::string prefix(TunerRecoveryJournalStagingDirectoryPrefix);
    if (name == prefix)
        return true;
    if (name.size() <= prefix.size() + 1U
        || name.compare(0U, prefix.size(), prefix) != 0
        || name[prefix.size()] != '.')
    {
        return false;
    }
    return std::all_of(
        name.begin() + static_cast<std::ptrdiff_t>(prefix.size() + 1U),
        name.end(),
        [](const unsigned char character) {
            return std::isdigit(character) != 0;
        });
}

} // namespace

ApplicationRecoveryDiscoveryResult DiscoverApplicationRecovery(
    const ApplicationStoragePaths& paths)
{
    ApplicationRecoveryDiscoveryResult result;

    if (paths.recoveryDirectory.empty())
    {
        return Blocked(
            ApplicationRecoveryBlockReason::StorageUnavailable,
            "Recovery is blocked because the application recovery path is "
            "empty. Discovery changed no files.");
    }

    std::error_code statusError;
    const auto directoryStatus = std::filesystem::symlink_status(
        paths.recoveryDirectory, statusError);
    if (directoryStatus.type() == std::filesystem::file_type::not_found
        || statusError == std::errc::no_such_file_or_directory)
    {
        result.state = ApplicationRecoveryDiscoveryState::Empty;
        result.message = "No application recovery record is present.";
        return result;
    }
    if (statusError)
    {
        return Blocked(
            ApplicationRecoveryBlockReason::StorageUnavailable,
            "Recovery is blocked because the application recovery directory "
            "could not be inspected: " + statusError.message()
                + ". Discovery changed no files.");
    }
    if (directoryStatus.type() != std::filesystem::file_type::directory)
    {
        return Blocked(
            ApplicationRecoveryBlockReason::StorageUnavailable,
            "Recovery is blocked because the application recovery path is "
            "not a plain directory. Discovery changed no files.");
    }

    std::error_code iterationError;
    std::filesystem::directory_iterator iterator(
        paths.recoveryDirectory, iterationError);
    const std::filesystem::directory_iterator end;
    while (!iterationError && iterator != end)
    {
        // CX-12F reserves these direct-child names for same-filesystem atomic
        // staging. A crash may abandon the directory and its partial child,
        // but neither is recovery evidence and discovery never descends into
        // or mutates it.
        if (!IsJournalStagingEntry(iterator->path()))
            result.recordPaths.push_back(iterator->path());
        iterator.increment(iterationError);
    }
    if (iterationError)
    {
        return Blocked(
            ApplicationRecoveryBlockReason::StorageUnavailable,
            "Recovery is blocked because the application recovery directory "
            "could not be read completely: " + iterationError.message()
                + ". Discovery changed no files.",
            std::move(result.recordPaths));
    }

    std::sort(result.recordPaths.begin(), result.recordPaths.end());
    if (result.recordPaths.empty())
    {
        result.state = ApplicationRecoveryDiscoveryState::Empty;
        result.message = "No application recovery record is present.";
        return result;
    }
    if (result.recordPaths.size() != 1U)
    {
        const auto names = EntryNames(result.recordPaths);
        return Blocked(
            ApplicationRecoveryBlockReason::MultipleRecords,
            "Recovery is blocked because the application recovery directory "
            "contains multiple entries (" + names
                + "). Discovery left every entry unchanged.",
            std::move(result.recordPaths));
    }

    const auto recordPath = result.recordPaths.front();
    std::error_code recordStatusError;
    const auto recordStatus = std::filesystem::symlink_status(
        recordPath, recordStatusError);
    if (recordStatusError
        || recordStatus.type() != std::filesystem::file_type::regular)
    {
        return Blocked(
            ApplicationRecoveryBlockReason::MalformedRecord,
            "Recovery is blocked because '"
                + recordPath.filename().string()
                + "' is not a plain recovery-journal file. Discovery left "
                  "it unchanged.",
            std::move(result.recordPaths));
    }

    auto loaded = LoadTunerRecoveryJournal(recordPath.string());
    if (!loaded.success || !loaded.record.has_value())
    {
        return Blocked(
            ApplicationRecoveryBlockReason::MalformedRecord,
            "Recovery is blocked because '"
                + recordPath.filename().string() + "' is malformed: "
                + loaded.message + " Discovery left it unchanged.",
            std::move(result.recordPaths));
    }

    if (loaded.record->formatVersion
            != TunerRecoveryJournalV5FormatVersion
        || !loaded.record->version5.has_value())
    {
        const auto version = loaded.record->formatVersion;
        return Blocked(
            ApplicationRecoveryBlockReason::UnsupportedLegacyVersion,
            "Recovery is blocked because '"
                + recordPath.filename().string() + "' uses journal version "
                + std::to_string(version)
                + ", which cannot supply the endpoint evidence required for "
                  "application-storage recovery. Discovery left it "
                  "unchanged.",
            std::move(result.recordPaths));
    }

    result.state = ApplicationRecoveryDiscoveryState::PendingV5;
    result.message =
        "Found one valid pending v5 application recovery record.";
    result.record = std::move(loaded.record);
    return result;
}

bool ApplicationRecoveryBlocksNormalTuning(
    const ApplicationRecoveryDiscoveryResult& discovery) noexcept
{
    return discovery.state == ApplicationRecoveryDiscoveryState::PendingV5
        || discovery.state == ApplicationRecoveryDiscoveryState::Blocked;
}

bool ApplicationRecoveryHasRetainedEvidence(
    const ApplicationRecoveryDiscoveryResult& discovery) noexcept
{
    return discovery.state == ApplicationRecoveryDiscoveryState::PendingV5
        || !discovery.recordPaths.empty();
}

} // namespace fidget
