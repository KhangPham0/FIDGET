#ifndef FIDGET_CORE_CRATE_PROJECT_H
#define FIDGET_CORE_CRATE_PROJECT_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fidget {

inline constexpr std::uint32_t CrateProjectFormatVersion = 1U;
inline constexpr std::size_t CrateProjectMaximumModules = 16U;

enum class MdppBackend : std::uint16_t
{
    Scp = 1U,
    Qdc = 2U,
};

struct CrateProjectModule
{
    std::string name;
    std::uint32_t baseAddress = 0U;
    MdppBackend backend = MdppBackend::Scp;
    std::string profilePath;
};

struct CrateProject
{
    std::uint32_t formatVersion = CrateProjectFormatVersion;
    std::string mvlcHost;
    std::uint16_t mvlcCommandPort = 32768U;
    std::string streamHost;
    std::uint16_t streamPort = 42333U;
    std::vector<CrateProjectModule> modules;
};

struct CrateProjectValidationResult
{
    bool success = false;
    std::string message;
};

struct CrateProjectSerializationResult
{
    bool success = false;
    std::string message;
    std::string text;
};

struct CrateProjectParseResult
{
    bool success = false;
    std::string message;
    std::optional<CrateProject> project;
};

struct CrateProjectSaveResult
{
    bool success = false;
    std::string message;
};

struct CrateProjectLoadResult
{
    bool success = false;
    std::string message;
    std::optional<CrateProject> project;
};

struct CrateProjectModuleTargetResult
{
    bool success = false;
    std::string message;
    std::optional<CrateProjectModule> module;
};

[[nodiscard]] const char* MdppBackendName(MdppBackend backend) noexcept;
[[nodiscard]] bool MdppBackendImplemented(MdppBackend backend) noexcept;

[[nodiscard]] CrateProjectValidationResult ValidateCrateProject(
    const CrateProject& project);

[[nodiscard]] CrateProjectModuleTargetResult ResolveCrateProjectModuleTarget(
    const CrateProject& project,
    std::size_t moduleIndex,
    MdppBackend requiredBackend);

[[nodiscard]] CrateProjectSerializationResult SerializeCrateProject(
    const CrateProject& project);

[[nodiscard]] CrateProjectParseResult ParseCrateProject(
    const std::string& text);

[[nodiscard]] CrateProjectSaveResult SaveCrateProject(
    const CrateProject& project,
    const std::string& path);

[[nodiscard]] CrateProjectLoadResult LoadCrateProject(
    const std::string& path);

} // namespace fidget

#endif
