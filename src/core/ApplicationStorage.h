#ifndef FIDGET_CORE_APPLICATION_STORAGE_H
#define FIDGET_CORE_APPLICATION_STORAGE_H

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>

namespace fidget {

inline constexpr std::size_t ApplicationHomeParentSearchLimit = 8U;

// fidget-state intentionally sits beside the build tree, or beside a
// standalone executable when no build tree can be found. Deleting it forfeits
// pending recovery evidence, the last verified host and module address,
// saved SSH convenience fields, window layout, logs, and reports. FIDGET
// never redirects this state into a user home directory.
struct ApplicationStoragePaths
{
    std::filesystem::path applicationHome;
    std::filesystem::path stateDirectory;
    std::filesystem::path preferencesFile;
    std::filesystem::path logsDirectory;
    std::filesystem::path recoveryDirectory;
    std::filesystem::path reportsDirectory;
    std::filesystem::path imguiIniFile;
    std::filesystem::path windowStateFile;
    std::filesystem::path layoutVersionFile;
};

struct ApplicationStorageResult
{
    bool success = false;
    std::string message;
};

using ApplicationStorageDirectorySynchronizer =
    std::function<bool(const std::string&, std::string&)>;

// Deterministic durability seam for tests. Production directory preparation
// fsyncs each storage directory and the parent entry that names it before
// reporting success.
struct ApplicationStorageDirectoryRuntime
{
    ApplicationStorageDirectorySynchronizer synchronize;
};

// The first two values may be updated only after a successful read-only target
// verification. The bridge fields are saved convenience values, not
// verification evidence. This structure deliberately has no password, key,
// token, or other secret field.
struct ApplicationPreferences
{
    std::string lastVerifiedEthernetHost;
    std::string lastVerifiedModuleAddress;
    std::string sshDestination;
    std::string remoteBridgeCommand;
};

struct ApplicationPreferencesLoadResult
{
    bool success = false;
    bool fileMissing = false;
    bool malformed = false;
    std::string message;
    ApplicationPreferences preferences;
};

[[nodiscard]] std::filesystem::path ExecutableDirectory();

[[nodiscard]] std::filesystem::path ResolveApplicationHome(
    const std::filesystem::path& executableDirectory,
    std::size_t parentSearchLimit = ApplicationHomeParentSearchLimit);

[[nodiscard]] std::filesystem::path ApplicationHome();

[[nodiscard]] ApplicationStoragePaths ApplicationStoragePathsForHome(
    const std::filesystem::path& applicationHome);

[[nodiscard]] ApplicationStoragePaths DefaultApplicationStoragePaths();

// Creates each missing level separately and reports success only after every
// storage directory and its parent directory entry have been synchronized.
// This durable preparation is required before a recovery journal may be used
// as authority for a state-changing operation.
[[nodiscard]] ApplicationStorageResult EnsureApplicationStorageDirectories(
    const ApplicationStoragePaths& paths,
    const ApplicationStorageDirectoryRuntime& runtime = {});

[[nodiscard]] ApplicationStorageResult SaveApplicationPreferences(
    const ApplicationStoragePaths& paths,
    const ApplicationPreferences& preferences);

[[nodiscard]] ApplicationPreferencesLoadResult LoadApplicationPreferences(
    const ApplicationStoragePaths& paths);

} // namespace fidget

#endif
