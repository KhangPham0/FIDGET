#include "core/CrateProject.h"

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <set>
#include <sstream>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace fidget {
namespace {

constexpr std::string_view ProjectMagic = "MWW_CRATE_PROJECT";
constexpr std::uint64_t FnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t FnvPrime = 1099511628211ULL;

void HashByte(std::uint64_t& hash, std::uint8_t value)
{
    hash ^= value;
    hash *= FnvPrime;
}

void HashValue(std::uint64_t& hash, std::uint32_t value)
{
    for (unsigned shift = 0U; shift < 32U; shift += 8U)
    {
        HashByte(hash, static_cast<std::uint8_t>(value >> shift));
    }
}

void HashText(std::uint64_t& hash, const std::string& value)
{
    HashValue(hash, static_cast<std::uint32_t>(value.size()));
    for (const unsigned char byte : value)
    {
        HashByte(hash, byte);
    }
}

std::uint64_t LegacyProjectChecksum(const CrateProject& project)
{
    std::uint64_t hash = FnvOffsetBasis;
    HashValue(hash, project.formatVersion);
    HashText(hash, project.mvlcHost);
    HashValue(hash, project.mvlcCommandPort);
    HashText(hash, project.streamHost);
    HashValue(hash, project.streamPort);
    HashValue(hash, static_cast<std::uint32_t>(project.modules.size()));
    for (const auto& module : project.modules)
    {
        HashText(hash, module.name);
        HashValue(hash, module.baseAddress);
        HashValue(hash, static_cast<std::uint16_t>(module.backend));
        HashText(hash, module.profilePath);
    }
    return hash;
}

std::uint64_t ProjectChecksum(const CrateProject& project)
{
    std::uint64_t hash = FnvOffsetBasis;
    HashValue(hash, project.formatVersion);
    HashValue(
        hash, static_cast<std::uint16_t>(project.endpointKind));
    HashText(hash, project.sshDestination);
    HashText(hash, project.remoteBridgeCommand);
    HashText(hash, project.mvlcHost);
    HashValue(hash, project.mvlcCommandPort);
    HashText(hash, project.streamHost);
    HashValue(hash, project.streamPort);
    HashValue(hash, static_cast<std::uint32_t>(project.modules.size()));
    for (const auto& module : project.modules)
    {
        HashText(hash, module.name);
        HashValue(hash, module.baseAddress);
        HashValue(hash, static_cast<std::uint16_t>(module.backend));
        HashText(hash, module.profilePath);
    }
    return hash;
}

bool ValidEndpointHost(const std::string& host)
{
    if (host.empty() || host.size() > 255U)
    {
        return false;
    }
    for (const unsigned char character : host)
    {
        if (character <= 0x20U || character == 0x7FU)
        {
            return false;
        }
    }
    return true;
}

bool ValidDisplayText(const std::string& text, std::size_t maximum)
{
    if (text.empty() || text.size() > maximum)
    {
        return false;
    }
    return std::all_of(
        text.begin(),
        text.end(),
        [](const unsigned char character) {
            return character >= 0x20U && character != 0x7FU;
        });
}

bool ValidSingleArgument(const std::string& text, std::size_t maximum)
{
    if (text.empty() || text.size() > maximum)
    {
        return false;
    }
    return std::all_of(
        text.begin(),
        text.end(),
        [](const unsigned char character) {
            return character > 0x20U && character != 0x7FU;
        });
}

bool ExpectToken(std::istream& input,
                 std::string_view expected,
                 std::string& error)
{
    std::string token;
    if (!(input >> token))
    {
        error = "Unexpected end of crate project while reading '"
            + std::string(expected) + "'.";
        return false;
    }
    if (token != expected)
    {
        error = "Expected crate-project token '" + std::string(expected)
            + "', found '" + token + "'.";
        return false;
    }
    return true;
}

template<typename Integer>
bool ReadUnsigned(std::istream& input,
                  Integer& destination,
                  const char* field,
                  std::string& error)
{
    static_assert(std::is_unsigned_v<Integer>);
    std::string token;
    if (!(input >> token))
    {
        error = std::string("Missing unsigned value for ") + field + '.';
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
            + "' for " + field + '.';
        return false;
    }
    destination = static_cast<Integer>(parsed);
    return true;
}

bool ReadQuotedText(std::istream& input,
                    std::string& destination,
                    const char* field,
                    std::string& error)
{
    if (!(input >> std::quoted(destination)))
    {
        error = std::string("Invalid quoted text for ") + field + '.';
        return false;
    }
    return true;
}

std::optional<MdppBackend> BackendFromValue(std::uint16_t value)
{
    switch (value)
    {
    case static_cast<std::uint16_t>(MdppBackend::Scp):
        return MdppBackend::Scp;
    case static_cast<std::uint16_t>(MdppBackend::Qdc):
        return MdppBackend::Qdc;
    default:
        return std::nullopt;
    }
}

std::optional<CrateProjectEndpointKind> EndpointKindFromValue(
    std::uint16_t value)
{
    switch (value)
    {
    case static_cast<std::uint16_t>(CrateProjectEndpointKind::Direct):
        return CrateProjectEndpointKind::Direct;
    case static_cast<std::uint16_t>(CrateProjectEndpointKind::SshBridge):
        return CrateProjectEndpointKind::SshBridge;
    default:
        return std::nullopt;
    }
}

} // namespace

const char* MdppBackendName(MdppBackend backend) noexcept
{
    switch (backend)
    {
    case MdppBackend::Scp:
        return "SCP";
    case MdppBackend::Qdc:
        return "QDC";
    }
    return "Unknown";
}

bool MdppBackendImplemented(MdppBackend backend) noexcept
{
    return backend == MdppBackend::Scp;
}

const char* CrateProjectEndpointKindName(
    CrateProjectEndpointKind kind) noexcept
{
    switch (kind)
    {
    case CrateProjectEndpointKind::Direct:
        return "Direct";
    case CrateProjectEndpointKind::SshBridge:
        return "SSH bridge";
    }
    return "Unknown";
}

CrateProjectValidationResult ValidateCrateProject(const CrateProject& project)
{
    CrateProjectValidationResult result;
    if (project.formatVersion != CrateProjectFormatVersion)
    {
        result.message = "Unsupported crate-project format version.";
        return result;
    }
    if (!EndpointKindFromValue(
            static_cast<std::uint16_t>(project.endpointKind)))
    {
        result.message = "The controller endpoint kind is unknown.";
        return result;
    }
    if (project.endpointKind == CrateProjectEndpointKind::SshBridge
        && (!ValidSingleArgument(project.sshDestination, 255U)
            || !ValidSingleArgument(
                project.remoteBridgeCommand, 511U)))
    {
        result.message =
            "The SSH bridge destination or remote command is invalid.";
        return result;
    }
    if (!ValidEndpointHost(project.mvlcHost)
        || project.mvlcCommandPort == 0U)
    {
        result.message = "The MVLC command endpoint is invalid.";
        return result;
    }
    if (!ValidEndpointHost(project.streamHost) || project.streamPort == 0U)
    {
        result.message = "The MVME Stream Server endpoint is invalid.";
        return result;
    }
    if (project.modules.empty()
        || project.modules.size() > CrateProjectMaximumModules)
    {
        result.message = "A crate project must contain between 1 and "
            + std::to_string(CrateProjectMaximumModules) + " modules.";
        return result;
    }

    std::set<std::string> names;
    std::set<std::uint32_t> bases;
    for (std::size_t index = 0; index < project.modules.size(); ++index)
    {
        const auto& module = project.modules[index];
        const std::string prefix =
            "Module " + std::to_string(index + 1U) + ": ";
        if (!ValidDisplayText(module.name, 63U))
        {
            result.message = prefix + "the display name is invalid.";
            return result;
        }
        if (!names.insert(module.name).second)
        {
            result.message = prefix + "module names must be unique.";
            return result;
        }
        if (module.baseAddress == 0U
            || (module.baseAddress & 0xFFFFU) != 0U)
        {
            result.message = prefix
                + "the A32 VME base must be nonzero and 64 KiB aligned.";
            return result;
        }
        if (!bases.insert(module.baseAddress).second)
        {
            result.message = prefix + "VME bases must be unique.";
            return result;
        }
        if (!BackendFromValue(
                static_cast<std::uint16_t>(module.backend)))
        {
            result.message = prefix + "the firmware backend is unknown.";
            return result;
        }
        if (!ValidDisplayText(module.profilePath, 511U))
        {
            result.message = prefix + "the profile path is invalid.";
            return result;
        }
    }

    result.success = true;
    result.message = "Crate project is valid: "
        + std::to_string(project.modules.size()) + " configured module(s).";
    return result;
}

CrateProjectModuleTargetResult ResolveCrateProjectModuleTarget(
    const CrateProject& project,
    std::size_t moduleIndex,
    MdppBackend requiredBackend)
{
    CrateProjectModuleTargetResult result;
    const auto validation = ValidateCrateProject(project);
    if (!validation.success)
    {
        result.message = "Cannot select a module from an invalid crate "
            "project: " + validation.message;
        return result;
    }
    if (moduleIndex >= project.modules.size())
    {
        result.message = "The active module index is outside the crate "
            "project inventory.";
        return result;
    }

    const auto& module = project.modules[moduleIndex];
    if (module.backend != requiredBackend)
    {
        result.message = "Module '" + module.name + "' uses the "
            + MdppBackendName(module.backend) + " backend; this operation "
            "requires " + MdppBackendName(requiredBackend) + ".";
        return result;
    }

    result.success = true;
    result.module = module;
    result.message = "Selected " + module.name + " at A32 base 0x";
    char address[9]{};
    const auto conversion = std::to_chars(
        address, address + sizeof(address) - 1U, module.baseAddress, 16);
    if (conversion.ec == std::errc{})
    {
        result.message.append(address, conversion.ptr);
    }
    else
    {
        result.message += "????????";
    }
    result.message += " for the "
        + std::string(MdppBackendName(requiredBackend))
        + " tuner backend.";
    return result;
}

CrateProjectSerializationResult SerializeCrateProject(
    const CrateProject& project)
{
    CrateProjectSerializationResult result;
    const auto validation = ValidateCrateProject(project);
    if (!validation.success)
    {
        result.message = validation.message;
        return result;
    }

    std::ostringstream output;
    output << ProjectMagic << ' ' << project.formatVersion << '\n';
    output << "ENDPOINT "
           << static_cast<std::uint16_t>(project.endpointKind) << ' '
           << std::quoted(project.sshDestination) << ' '
           << std::quoted(project.remoteBridgeCommand) << '\n';
    output << "MVLC " << std::quoted(project.mvlcHost) << ' '
           << project.mvlcCommandPort << '\n';
    output << "STREAM " << std::quoted(project.streamHost) << ' '
           << project.streamPort << '\n';
    output << "MODULES " << project.modules.size() << '\n';
    for (const auto& module : project.modules)
    {
        output << "MODULE " << std::quoted(module.name) << ' '
               << module.baseAddress << ' '
               << static_cast<std::uint16_t>(module.backend) << ' '
               << std::quoted(module.profilePath) << '\n';
    }
    output << "CHECKSUM " << ProjectChecksum(project) << '\n';
    output << "END\n";

    if (!output)
    {
        result.message = "Writing the crate-project text failed.";
        return result;
    }

    result.success = true;
    result.text = output.str();
    return result;
}

CrateProjectParseResult ParseCrateProject(const std::string& text)
{
    CrateProjectParseResult result;
    std::istringstream input(text);
    CrateProject project;
    std::string error;
    std::size_t moduleCount = 0U;
    std::uint64_t storedChecksum = 0U;
    std::uint16_t endpointKindValue = 0U;

    if (!ExpectToken(input, ProjectMagic, error)
        || !ReadUnsigned(
            input, project.formatVersion, "format version", error))
    {
        result.message = "Invalid crate project: " + error;
        return result;
    }

    const bool legacyVersion = project.formatVersion == 1U;
    if (!legacyVersion && project.formatVersion != CrateProjectFormatVersion)
    {
        result.message = "Invalid crate project: Unsupported crate-project "
            "format version.";
        return result;
    }

    if (!legacyVersion)
    {
        if (!ExpectToken(input, "ENDPOINT", error)
            || !ReadUnsigned(
                input, endpointKindValue, "endpoint kind", error)
            || !ReadQuotedText(
                input, project.sshDestination, "SSH destination", error)
            || !ReadQuotedText(
                input,
                project.remoteBridgeCommand,
                "remote bridge command",
                error))
        {
            result.message = "Invalid crate project: " + error;
            return result;
        }
        const auto endpointKind = EndpointKindFromValue(endpointKindValue);
        if (!endpointKind)
        {
            result.message =
                "Invalid crate project: unknown endpoint kind value "
                + std::to_string(endpointKindValue) + '.';
            return result;
        }
        project.endpointKind = *endpointKind;
    }

    if (!ExpectToken(input, "MVLC", error)
        || !ReadQuotedText(input, project.mvlcHost, "MVLC host", error)
        || !ReadUnsigned(
            input, project.mvlcCommandPort, "MVLC command port", error)
        || !ExpectToken(input, "STREAM", error)
        || !ReadQuotedText(input, project.streamHost, "stream host", error)
        || !ReadUnsigned(input, project.streamPort, "stream port", error)
        || !ExpectToken(input, "MODULES", error)
        || !ReadUnsigned(input, moduleCount, "module count", error))
    {
        result.message = "Invalid crate project: " + error;
        return result;
    }

    if (moduleCount == 0U || moduleCount > CrateProjectMaximumModules)
    {
        result.message = "Invalid crate project: module count is outside "
            "the supported range.";
        return result;
    }

    project.modules.reserve(moduleCount);
    for (std::size_t index = 0; index < moduleCount; ++index)
    {
        CrateProjectModule module;
        std::uint16_t backendValue = 0U;
        if (!ExpectToken(input, "MODULE", error)
            || !ReadQuotedText(input, module.name, "module name", error)
            || !ReadUnsigned(input, module.baseAddress, "VME base", error)
            || !ReadUnsigned(
                input, backendValue, "firmware backend", error)
            || !ReadQuotedText(
                input, module.profilePath, "profile path", error))
        {
            result.message = "Invalid crate project: " + error;
            return result;
        }

        const auto backend = BackendFromValue(backendValue);
        if (!backend)
        {
            result.message = "Invalid crate project: unknown firmware "
                "backend value " + std::to_string(backendValue) + '.';
            return result;
        }
        module.backend = *backend;
        project.modules.push_back(std::move(module));
    }

    if (!ExpectToken(input, "CHECKSUM", error)
        || !ReadUnsigned(
            input, storedChecksum, "crate-project checksum", error)
        || !ExpectToken(input, "END", error))
    {
        result.message = "Invalid crate project: " + error;
        return result;
    }

    std::string trailingToken;
    if (input >> trailingToken)
    {
        result.message = "Invalid crate project: unexpected trailing token '"
            + trailingToken + "'.";
        return result;
    }

    const std::uint64_t expectedChecksum = legacyVersion
        ? LegacyProjectChecksum(project)
        : ProjectChecksum(project);
    if (storedChecksum != expectedChecksum)
    {
        result.message = "Invalid crate project: checksum mismatch; the file "
            "is damaged or was modified outside the workbench.";
        return result;
    }

    if (legacyVersion)
    {
        project.formatVersion = CrateProjectFormatVersion;
        project.endpointKind = CrateProjectEndpointKind::Direct;
        project.sshDestination.clear();
        project.remoteBridgeCommand = "fidget_bridge";
    }
    const auto validation = ValidateCrateProject(project);
    if (!validation.success)
    {
        result.message = "Invalid crate project: " + validation.message;
        return result;
    }

    result.success = true;
    result.message = "Parsed crate project with "
        + std::to_string(project.modules.size()) + " module(s).";
    result.project = std::move(project);
    return result;
}

CrateProjectSaveResult SaveCrateProject(const CrateProject& project,
                                        const std::string& path)
{
    CrateProjectSaveResult result;
    if (path.empty())
    {
        result.message = "The crate-project path is empty.";
        return result;
    }

    const auto serialized = SerializeCrateProject(project);
    if (!serialized.success)
    {
        result.message = "Crate project not saved: " + serialized.message;
        return result;
    }

    const std::string temporary = path + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        result.message = "Could not open temporary crate project '"
            + temporary + "'.";
        return result;
    }

    output.write(serialized.text.data(),
                 static_cast<std::streamsize>(serialized.text.size()));
    output.flush();
    output.close();

    if (!output)
    {
        std::remove(temporary.c_str());
        result.message = "Writing the crate project failed.";
        return result;
    }

    if (std::rename(temporary.c_str(), path.c_str()) != 0)
    {
        const std::string renameError = std::strerror(errno);
        std::remove(temporary.c_str());
        result.message = "Could not install the completed crate project: "
            + renameError + '.';
        return result;
    }

    result.success = true;
    result.message = "Saved crate project to '" + path + "'.";
    return result;
}

CrateProjectLoadResult LoadCrateProject(const std::string& path)
{
    CrateProjectLoadResult result;
    if (path.empty())
    {
        result.message = "The crate-project path is empty.";
        return result;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        result.message = "Could not open crate project '" + path + "'.";
        return result;
    }

    std::ostringstream contents;
    contents << input.rdbuf();
    if (input.bad())
    {
        result.message = "Could not read crate project '" + path + "'.";
        return result;
    }

    auto parsed = ParseCrateProject(contents.str());
    if (!parsed.success || !parsed.project)
    {
        result.message = parsed.message;
        return result;
    }

    result.success = true;
    result.message = "Loaded crate project '" + path + "' with "
        + std::to_string(parsed.project->modules.size()) + " module(s).";
    result.project = std::move(parsed.project);
    return result;
}

} // namespace fidget
