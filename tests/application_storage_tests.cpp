#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/ApplicationStorage.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

using namespace fidget;

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        const auto unique = std::chrono::steady_clock::now()
                                .time_since_epoch().count();
        for (std::size_t index = 0U; index < 1000U; ++index)
        {
            const auto candidate = std::filesystem::temp_directory_path()
                / ("fidget-application-storage-tests-"
                   + std::to_string(unique) + '-'
                   + std::to_string(index));
            std::error_code createError;
            if (std::filesystem::create_directory(candidate, createError))
            {
                path_ = std::filesystem::weakly_canonical(candidate);
                break;
            }
            REQUIRE_FALSE(createError);
        }
        REQUIRE_FALSE(path_.empty());
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& Get() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void WriteText(
    const std::filesystem::path& path,
    const std::string& text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output << text;
    output.close();
    REQUIRE(output.good());
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    std::ostringstream contents;
    contents << input.rdbuf();
    REQUIRE_FALSE(input.bad());
    return contents.str();
}

ApplicationPreferences ExamplePreferences()
{
    ApplicationPreferences preferences;
    preferences.lastVerifiedEthernetHost = "mvlc.example.invalid";
    preferences.lastVerifiedModuleAddress = "0x1100";
    preferences.sshDestination = "daq-bridge";
    preferences.remoteBridgeCommand = "fidget_bridge";
    return preferences;
}

void CheckPreferences(
    const ApplicationPreferences& actual,
    const ApplicationPreferences& expected)
{
    CHECK(actual.lastVerifiedEthernetHost
          == expected.lastVerifiedEthernetHost);
    CHECK(actual.lastVerifiedModuleAddress
          == expected.lastVerifiedModuleAddress);
    CHECK(actual.sshDestination == expected.sshDestination);
    CHECK(actual.remoteBridgeCommand == expected.remoteBridgeCommand);
}

} // namespace

TEST_CASE("application home resolves above the nearest build root")
{
    TemporaryDirectory temporary;
    const auto repository = temporary.Get() / "source";
    const auto buildRoot = repository / "out";
    const auto executableDirectory = buildRoot / "src" / "tools";
    REQUIRE(std::filesystem::create_directories(executableDirectory));
    WriteText(buildRoot / "CMakeCache.txt", "test marker\n");

    CHECK(ResolveApplicationHome(executableDirectory) == repository);

    const auto nestedBuild = buildRoot / "src";
    WriteText(nestedBuild / "CMakeCache.txt", "nearer marker\n");
    CHECK(ResolveApplicationHome(executableDirectory)
          == nestedBuild.parent_path());
}

TEST_CASE("application home falls back to the executable directory")
{
    TemporaryDirectory temporary;
    const auto standalone = temporary.Get() / "standalone";
    REQUIRE(std::filesystem::create_directories(standalone));
    CHECK(ResolveApplicationHome(standalone) == standalone);

    const auto buildRoot = temporary.Get() / "distant-build";
    const auto executableDirectory =
        buildRoot / "one" / "two" / "three";
    REQUIRE(std::filesystem::create_directories(executableDirectory));
    WriteText(buildRoot / "CMakeCache.txt", "test marker\n");
    CHECK(ResolveApplicationHome(executableDirectory, 1U)
          == executableDirectory);
}

TEST_CASE("storage paths remain under fidget-state beside the build tree")
{
    TemporaryDirectory temporary;
    const auto paths = ApplicationStoragePathsForHome(temporary.Get());
    const auto state = temporary.Get() / "fidget-state";

    CHECK(paths.applicationHome == temporary.Get());
    CHECK(paths.stateDirectory == state);
    CHECK(paths.preferencesFile == state / "preferences.ini");
    CHECK(paths.logsDirectory == state / "logs");
    CHECK(paths.recoveryDirectory == state / "recovery");
    CHECK(paths.reportsDirectory == state / "reports");
    CHECK(paths.imguiIniFile == state / "imgui.ini");
    CHECK(paths.windowStateFile == state / "window.ini");
    CHECK(paths.layoutVersionFile == state / "layout.version");

    const auto ready = EnsureApplicationStorageDirectories(paths);
    REQUIRE(ready.success);
    CHECK(std::filesystem::is_directory(paths.stateDirectory));
    CHECK(std::filesystem::is_directory(paths.logsDirectory));
    CHECK(std::filesystem::is_directory(paths.recoveryDirectory));
    CHECK(std::filesystem::is_directory(paths.reportsDirectory));
}

TEST_CASE("missing and malformed preferences fail without guessed values")
{
    TemporaryDirectory temporary;
    const auto paths = ApplicationStoragePathsForHome(temporary.Get());

    const auto missing = LoadApplicationPreferences(paths);
    CHECK_FALSE(missing.success);
    CHECK(missing.fileMissing);
    CHECK_FALSE(missing.malformed);
    CheckPreferences(missing.preferences, {});

    REQUIRE(EnsureApplicationStorageDirectories(paths).success);
    WriteText(
        paths.preferencesFile,
        "FIDGET_PREFERENCES 1\n"
        "ETHERNET_HOST \"mvlc.example.invalid\"\n"
        "MODULE_ADDRESS \"0x1100\"\n"
        "SSH_DESTINATION \"daq-bridge\"\n"
        "END\n");
    const auto malformed = LoadApplicationPreferences(paths);
    CHECK_FALSE(malformed.success);
    CHECK_FALSE(malformed.fileMissing);
    CHECK(malformed.malformed);
    CheckPreferences(malformed.preferences, {});
}

TEST_CASE("preferences replace atomically and contain only approved fields")
{
    TemporaryDirectory temporary;
    const auto paths = ApplicationStoragePathsForHome(temporary.Get());
    auto preferences = ExamplePreferences();

    REQUIRE(SaveApplicationPreferences(paths, preferences).success);
    auto loaded = LoadApplicationPreferences(paths);
    REQUIRE(loaded.success);
    CheckPreferences(loaded.preferences, preferences);

    preferences.lastVerifiedEthernetHost = "replacement.example.invalid";
    preferences.lastVerifiedModuleAddress = "0x2200";
    preferences.sshDestination = "replacement-bridge";
    preferences.remoteBridgeCommand = "fidget_bridge";
    REQUIRE(SaveApplicationPreferences(paths, preferences).success);
    loaded = LoadApplicationPreferences(paths);
    REQUIRE(loaded.success);
    CheckPreferences(loaded.preferences, preferences);

    const auto serialized = ReadText(paths.preferencesFile);
    CHECK(serialized.find("ETHERNET_HOST") != std::string::npos);
    CHECK(serialized.find("MODULE_ADDRESS") != std::string::npos);
    CHECK(serialized.find("SSH_DESTINATION") != std::string::npos);
    CHECK(serialized.find("REMOTE_BRIDGE_COMMAND") != std::string::npos);
    CHECK(serialized.find("PASSWORD") == std::string::npos);
    CHECK(serialized.find("TOKEN") == std::string::npos);
    CHECK_FALSE(std::filesystem::exists(
        paths.preferencesFile.string() + ".tmp"));
}

TEST_CASE("atomic preference saves tolerate a temporary-path collision")
{
    TemporaryDirectory temporary;
    const auto paths = ApplicationStoragePathsForHome(temporary.Get());
    REQUIRE(EnsureApplicationStorageDirectories(paths).success);

    auto collision = paths.preferencesFile;
    collision += ".tmp";
    REQUIRE(std::filesystem::create_directory(collision));
    WriteText(collision / "owned-by-another-save", "leave intact\n");

    const auto preferences = ExamplePreferences();
    REQUIRE(SaveApplicationPreferences(paths, preferences).success);
    REQUIRE(std::filesystem::exists(
        collision / "owned-by-another-save"));
    CHECK(ReadText(collision / "owned-by-another-save")
          == "leave intact\n");
    const auto loaded = LoadApplicationPreferences(paths);
    REQUIRE(loaded.success);
    CheckPreferences(loaded.preferences, preferences);
}

TEST_CASE("an unwritable application home fails without side effects")
{
    TemporaryDirectory temporary;

    const auto nonDirectoryHome = temporary.Get() / "not-a-directory";
    WriteText(nonDirectoryHome, "blocking file\n");
    const auto blockedPaths =
        ApplicationStoragePathsForHome(nonDirectoryHome);
    CHECK_FALSE(SaveApplicationPreferences(
        blockedPaths, ExamplePreferences()).success);
    CHECK_FALSE(std::filesystem::exists(blockedPaths.stateDirectory));
}

TEST_CASE("a storage directory collision is retained and reported")
{
    TemporaryDirectory temporary;
    const auto paths = ApplicationStoragePathsForHome(temporary.Get());
    REQUIRE(std::filesystem::create_directory(paths.stateDirectory));
    WriteText(paths.logsDirectory, "directory collision\n");
    const auto ready = EnsureApplicationStorageDirectories(paths);
    CHECK_FALSE(ready.success);
    CHECK(ReadText(paths.logsDirectory) == "directory collision\n");
}
