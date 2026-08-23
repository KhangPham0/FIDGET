#include "core/ApplicationStorage.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace fidget {
namespace {

constexpr const char* PreferencesMagic = "FIDGET_PREFERENCES";
constexpr unsigned int PreferencesFormatVersion = 1U;
constexpr std::size_t MaximumPreferenceLength = 4096U;
constexpr std::size_t MaximumTemporaryCollisions = 64U;

std::filesystem::path NormalizePath(const std::filesystem::path& path)
{
    if (path.empty())
        return {};

    std::error_code error;
    auto normalized = std::filesystem::weakly_canonical(path, error);
    return error ? path.lexically_normal() : normalized;
}

ApplicationStoragePaths BuildStoragePaths(
    const std::filesystem::path& applicationHome)
{
    ApplicationStoragePaths paths;
    paths.applicationHome = NormalizePath(applicationHome);
    if (paths.applicationHome.empty())
        return paths;

    paths.stateDirectory = paths.applicationHome / "fidget-state";
    paths.preferencesFile = paths.stateDirectory / "preferences.ini";
    paths.logsDirectory = paths.stateDirectory / "logs";
    paths.recoveryDirectory = paths.stateDirectory / "recovery";
    paths.reportsDirectory = paths.stateDirectory / "reports";
    paths.imguiIniFile = paths.stateDirectory / "imgui.ini";
    paths.windowStateFile = paths.stateDirectory / "window.ini";
    paths.layoutVersionFile = paths.stateDirectory / "layout.version";
    return paths;
}

bool StorageLayoutMatches(const ApplicationStoragePaths& paths)
{
    if (paths.applicationHome.empty())
        return false;

    const auto expected = BuildStoragePaths(paths.applicationHome);
    return paths.applicationHome == expected.applicationHome
        && paths.stateDirectory == expected.stateDirectory
        && paths.preferencesFile == expected.preferencesFile
        && paths.logsDirectory == expected.logsDirectory
        && paths.recoveryDirectory == expected.recoveryDirectory
        && paths.reportsDirectory == expected.reportsDirectory
        && paths.imguiIniFile == expected.imguiIniFile
        && paths.windowStateFile == expected.windowStateFile
        && paths.layoutVersionFile == expected.layoutVersionFile;
}

bool RequireExistingDirectory(
    const std::filesystem::path& path,
    const char* label,
    std::string& error)
{
    std::error_code statusError;
    const auto status = std::filesystem::symlink_status(path, statusError);
    if (statusError || status.type() != std::filesystem::file_type::directory)
    {
        error = std::string("The ") + label
            + " is not an existing plain directory.";
        return false;
    }
    return true;
}

bool EnsurePlainDirectory(
    const std::filesystem::path& path,
    const char* label,
    std::string& error)
{
    std::error_code existsError;
    const bool exists = std::filesystem::exists(path, existsError);
    if (existsError)
    {
        error = std::string("Could not inspect the ") + label + ": "
            + existsError.message() + '.';
        return false;
    }

    if (exists)
        return RequireExistingDirectory(path, label, error);

    std::error_code createError;
    std::filesystem::create_directories(path, createError);
    if (createError)
    {
        error = std::string("Could not create the ") + label + ": "
            + createError.message() + '.';
        return false;
    }
    return RequireExistingDirectory(path, label, error);
}

bool IsValidPreferenceValue(const std::string& value)
{
    if (value.size() > MaximumPreferenceLength)
        return false;

    return std::all_of(
        value.begin(), value.end(),
        [](const char valueChar) {
            const auto byte = static_cast<unsigned char>(valueChar);
            return byte >= 0x20U && byte != 0x7fU;
        });
}

bool ValidatePreferences(
    const ApplicationPreferences& preferences,
    std::string& error)
{
    const std::array<const std::string*, 4U> values{{
        &preferences.lastVerifiedEthernetHost,
        &preferences.lastVerifiedModuleAddress,
        &preferences.sshDestination,
        &preferences.remoteBridgeCommand,
    }};
    if (std::all_of(
            values.begin(), values.end(),
            [](const std::string* value) {
                return IsValidPreferenceValue(*value);
            }))
    {
        return true;
    }

    error = "Application preferences contain an invalid or oversized value.";
    return false;
}

std::string SerializePreferences(const ApplicationPreferences& preferences)
{
    std::ostringstream output;
    output << PreferencesMagic << ' ' << PreferencesFormatVersion << '\n'
           << "ETHERNET_HOST "
           << std::quoted(preferences.lastVerifiedEthernetHost) << '\n'
           << "MODULE_ADDRESS "
           << std::quoted(preferences.lastVerifiedModuleAddress) << '\n'
           << "SSH_DESTINATION "
           << std::quoted(preferences.sshDestination) << '\n'
           << "REMOTE_BRIDGE_COMMAND "
           << std::quoted(preferences.remoteBridgeCommand) << '\n'
           << "END\n";
    return output.str();
}

bool ReadPreferenceField(
    std::istream& input,
    const char* expectedLabel,
    std::string& value)
{
    std::string label;
    return static_cast<bool>(input >> label)
        && label == expectedLabel
        && static_cast<bool>(input >> std::quoted(value));
}

bool ParsePreferences(
    const std::string& text,
    ApplicationPreferences& preferences,
    std::string& error)
{
    std::istringstream input(text);
    std::string magic;
    unsigned int version = 0U;
    if (!(input >> magic >> version)
        || magic != PreferencesMagic
        || version != PreferencesFormatVersion)
    {
        error = "Application preferences have an unsupported header.";
        return false;
    }

    if (!ReadPreferenceField(
            input, "ETHERNET_HOST",
            preferences.lastVerifiedEthernetHost)
        || !ReadPreferenceField(
            input, "MODULE_ADDRESS",
            preferences.lastVerifiedModuleAddress)
        || !ReadPreferenceField(
            input, "SSH_DESTINATION",
            preferences.sshDestination)
        || !ReadPreferenceField(
            input, "REMOTE_BRIDGE_COMMAND",
            preferences.remoteBridgeCommand))
    {
        error = "Application preferences are incomplete or malformed.";
        return false;
    }

    std::string end;
    if (!(input >> end) || end != "END")
    {
        error = "Application preferences are missing the end marker.";
        return false;
    }
    input >> std::ws;
    if (!input.eof())
    {
        error = "Application preferences contain trailing data.";
        return false;
    }
    return ValidatePreferences(preferences, error);
}

void RemoveTemporaryDirectory(const std::filesystem::path& path)
{
    std::error_code removeError;
    std::filesystem::remove_all(path, removeError);
}

ApplicationStorageResult AtomicWrite(
    const std::filesystem::path& destination,
    const std::string& contents)
{
    ApplicationStorageResult result;

    std::error_code destinationError;
    if (std::filesystem::exists(destination, destinationError))
    {
        const auto status = std::filesystem::symlink_status(
            destination, destinationError);
        if (destinationError
            || status.type() != std::filesystem::file_type::regular)
        {
            result.message =
                "The application preferences destination is not a plain file.";
            return result;
        }
    }
    else if (destinationError)
    {
        result.message = "Could not inspect the application preferences file: "
            + destinationError.message() + '.';
        return result;
    }

    std::filesystem::path temporaryDirectory;
    for (std::size_t index = 0U;
         index < MaximumTemporaryCollisions; ++index)
    {
        temporaryDirectory = destination;
        temporaryDirectory += index == 0U
            ? ".tmp"
            : ".tmp." + std::to_string(index);

        std::error_code createError;
        if (std::filesystem::create_directory(
                temporaryDirectory, createError))
        {
            break;
        }
        if (createError)
        {
            std::error_code collisionError;
            if (!std::filesystem::exists(
                    temporaryDirectory, collisionError)
                || collisionError)
            {
                result.message =
                    "Could not create an atomic preferences workspace: "
                    + createError.message() + '.';
                return result;
            }
        }
        temporaryDirectory.clear();
    }

    if (temporaryDirectory.empty())
    {
        result.message =
            "Could not allocate an atomic preferences workspace.";
        return result;
    }

    const auto temporaryFile = temporaryDirectory / "preferences";
    std::ofstream output(
        temporaryFile, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        RemoveTemporaryDirectory(temporaryDirectory);
        result.message = "Could not open temporary application preferences.";
        return result;
    }

    output.write(
        contents.data(), static_cast<std::streamsize>(contents.size()));
    output.flush();
    output.close();
    if (!output)
    {
        RemoveTemporaryDirectory(temporaryDirectory);
        result.message = "Writing application preferences failed.";
        return result;
    }

    std::error_code renameError;
    std::filesystem::rename(temporaryFile, destination, renameError);
    if (renameError)
    {
        RemoveTemporaryDirectory(temporaryDirectory);
        result.message = "Could not atomically install application preferences: "
            + renameError.message() + '.';
        return result;
    }
    RemoveTemporaryDirectory(temporaryDirectory);

    result.success = true;
    result.message = "Saved application preferences.";
    return result;
}

} // namespace

std::filesystem::path ExecutableDirectory()
{
#ifdef __APPLE__
    std::array<char, 4096U> buffer{};
    std::uint32_t size = static_cast<std::uint32_t>(buffer.size());
    if (_NSGetExecutablePath(buffer.data(), &size) == 0)
        return NormalizePath(buffer.data()).parent_path();

    std::vector<char> expanded(size);
    if (_NSGetExecutablePath(expanded.data(), &size) == 0)
        return NormalizePath(expanded.data()).parent_path();
#else
    std::error_code error;
    const auto executable = std::filesystem::read_symlink(
        "/proc/self/exe", error);
    if (!error)
        return NormalizePath(executable).parent_path();
#endif
    return {};
}

std::filesystem::path ResolveApplicationHome(
    const std::filesystem::path& executableDirectory,
    const std::size_t parentSearchLimit)
{
    const auto fallback = NormalizePath(executableDirectory);
    if (fallback.empty())
        return {};

    auto candidate = fallback;
    for (std::size_t depth = 0U; depth <= parentSearchLimit; ++depth)
    {
        std::error_code markerError;
        const bool markerFound = std::filesystem::is_regular_file(
            candidate / "CMakeCache.txt", markerError);
        if (!markerError && markerFound)
            return candidate.parent_path();

        const auto parent = candidate.parent_path();
        if (parent.empty() || parent == candidate)
            break;
        candidate = parent;
    }
    return fallback;
}

std::filesystem::path ApplicationHome()
{
    return ResolveApplicationHome(ExecutableDirectory());
}

ApplicationStoragePaths ApplicationStoragePathsForHome(
    const std::filesystem::path& applicationHome)
{
    return BuildStoragePaths(applicationHome);
}

ApplicationStoragePaths DefaultApplicationStoragePaths()
{
    return ApplicationStoragePathsForHome(ApplicationHome());
}

ApplicationStorageResult EnsureApplicationStorageDirectories(
    const ApplicationStoragePaths& paths)
{
    ApplicationStorageResult result;
    if (!StorageLayoutMatches(paths))
    {
        result.message = "The application storage layout is invalid.";
        return result;
    }

    std::string error;
    if (!RequireExistingDirectory(
            paths.applicationHome, "application home", error)
        || !EnsurePlainDirectory(
            paths.stateDirectory, "application state directory", error)
        || !EnsurePlainDirectory(
            paths.logsDirectory, "application log directory", error)
        || !EnsurePlainDirectory(
            paths.recoveryDirectory,
            "application recovery directory", error)
        || !EnsurePlainDirectory(
            paths.reportsDirectory,
            "application report directory", error))
    {
        result.message = std::move(error);
        return result;
    }

    result.success = true;
    result.message = "Application storage is ready.";
    return result;
}

ApplicationStorageResult SaveApplicationPreferences(
    const ApplicationStoragePaths& paths,
    const ApplicationPreferences& preferences)
{
    std::string validationError;
    if (!ValidatePreferences(preferences, validationError))
        return {false, std::move(validationError)};

    const auto ready = EnsureApplicationStorageDirectories(paths);
    if (!ready.success)
        return ready;

    return AtomicWrite(paths.preferencesFile, SerializePreferences(preferences));
}

ApplicationPreferencesLoadResult LoadApplicationPreferences(
    const ApplicationStoragePaths& paths)
{
    ApplicationPreferencesLoadResult result;
    if (!StorageLayoutMatches(paths))
    {
        result.message = "The application storage layout is invalid.";
        return result;
    }

    std::ifstream input(paths.preferencesFile, std::ios::binary);
    if (!input)
    {
        std::error_code existsError;
        const bool exists = std::filesystem::exists(
            paths.preferencesFile, existsError);
        result.fileMissing = !exists && !existsError;
        result.message = result.fileMissing
            ? "No application preferences are present."
            : "Could not open application preferences.";
        return result;
    }

    std::ostringstream contents;
    contents << input.rdbuf();
    if (input.bad())
    {
        result.message = "Could not read application preferences.";
        return result;
    }

    std::string parseError;
    if (!ParsePreferences(
            contents.str(), result.preferences, parseError))
    {
        result.malformed = true;
        result.message = std::move(parseError);
        result.preferences = {};
        return result;
    }

    result.success = true;
    result.message = "Loaded application preferences.";
    return result;
}

} // namespace fidget
