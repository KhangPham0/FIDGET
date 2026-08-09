#include "core/ScpProfile.h"

#include "core/ScpRegistry.h"

#include <cerrno>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace fidget {
namespace {

constexpr std::string_view ProfileMagic = "MWW_SCP_PROFILE";
constexpr std::uint64_t FnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t FnvPrime = 1099511628211ULL;

void HashValue(std::uint64_t& hash, std::uint32_t value)
{
    for (unsigned shift = 0U; shift < 32U; shift += 8U)
    {
        hash ^= (value >> shift) & 0xFFU;
        hash *= FnvPrime;
    }
}

std::uint64_t ConfigurationChecksum(
    const Fw2051ScpConfigurationSnapshot& configuration)
{
    std::uint64_t hash = FnvOffsetBasis;
    HashValue(hash, Fw2051ScpProfileFormatVersion);
    HashValue(hash, configuration.baseAddress);
    HashValue(hash, configuration.hardwareId);
    HashValue(hash, configuration.firmwareRevision);
    HashValue(hash, configuration.irqLevel);
    HashValue(hash, configuration.outputFormat);
    for (const auto& quad : configuration.quads)
    {
        HashValue(hash, quad.quad);
        HashValue(hash, quad.timingFilter);
        for (const auto value : quad.poleZero)
        {
            HashValue(hash, value);
        }
        HashValue(hash, quad.gain);
        for (const auto value : quad.thresholds)
        {
            HashValue(hash, value);
        }
        HashValue(hash, quad.shapingTime);
        HashValue(hash, quad.baselineRestorer);
        HashValue(hash, quad.resetTime);
        HashValue(hash, quad.signalRiseTime);
        HashValue(hash, quad.preSamples);
        HashValue(hash, quad.totalSamples);
        HashValue(hash, quad.sampleConfiguration);
    }
    return hash;
}

bool ExpectToken(std::istream& input,
                 std::string_view expected,
                 std::string& error)
{
    std::string token;
    if (!(input >> token))
    {
        error = "Unexpected end of profile while reading '"
            + std::string(expected) + "'.";
        return false;
    }

    if (token != expected)
    {
        error = "Expected profile token '" + std::string(expected)
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

void WriteQuad(std::ostream& output,
               const Fw2051ScpQuadConfiguration& quad)
{
    output << "QUAD " << quad.quad << ' ' << quad.timingFilter;
    for (const auto value : quad.poleZero)
    {
        output << ' ' << value;
    }
    output << ' ' << quad.gain;
    for (const auto value : quad.thresholds)
    {
        output << ' ' << value;
    }
    output << ' ' << quad.shapingTime
           << ' ' << quad.baselineRestorer
           << ' ' << quad.resetTime
           << ' ' << quad.signalRiseTime
           << ' ' << quad.preSamples
           << ' ' << quad.totalSamples
           << ' ' << quad.sampleConfiguration << '\n';
}

bool ReadQuad(std::istream& input,
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
        {
            return false;
        }
    }

    if (!ReadUnsigned(input, quad.gain, "gain", error))
    {
        return false;
    }
    for (auto& value : quad.thresholds)
    {
        if (!ReadUnsigned(input, value, "threshold", error))
        {
            return false;
        }
    }

    return ReadUnsigned(input, quad.shapingTime, "shaping time", error)
        && ReadUnsigned(
            input, quad.baselineRestorer, "baseline restorer", error)
        && ReadUnsigned(input, quad.resetTime, "reset time", error)
        && ReadUnsigned(input, quad.signalRiseTime, "signal rise time", error)
        && ReadUnsigned(input, quad.preSamples, "pre-samples", error)
        && ReadUnsigned(input, quad.totalSamples, "total samples", error)
        && ReadUnsigned(
            input,
            quad.sampleConfiguration,
            "sample configuration",
            error);
}

void AddDifference(ScpConfigurationComparison& comparison,
                   int quad,
                   bool hasRegister,
                   std::uint32_t registerOffset,
                   std::string setting,
                   std::uint32_t profileValue,
                   std::uint32_t liveValue,
                   bool displayHexadecimal = false)
{
    ++comparison.valuesCompared;
    if (profileValue == liveValue)
    {
        return;
    }

    comparison.differences.push_back(ScpConfigurationDifference{
        quad,
        hasRegister,
        registerOffset,
        std::move(setting),
        profileValue,
        liveValue,
        displayHexadecimal,
    });
}

} // namespace

std::string ValidateFw2051ScpConfiguration(
    const Fw2051ScpConfigurationSnapshot& configuration)
{
    if (configuration.state != ScpConfigurationState::Complete)
    {
        return "The SCP snapshot is not complete.";
    }

    if (!configuration.selectorParkedAtQuadZero)
    {
        return "The SCP selector was not verified at quad 0.";
    }

    if (configuration.baseAddress == 0U)
    {
        return "The SCP snapshot has no VME base address.";
    }

    if (configuration.hardwareId != Mdpp32HardwareId
        && configuration.hardwareId != Mdpp32AlternateHardwareId)
    {
        return "The snapshot does not identify as an MDPP-32 module.";
    }

    if (((configuration.firmwareRevision >> 12U) & 0x0FU) != 2U)
    {
        return "The snapshot does not report SCP firmware.";
    }

    if (configuration.firmwareRevision
        != Mdpp32ScpFirmwareRevisionFw2051)
    {
        return "The snapshot does not report supported SCP FW2051 firmware.";
    }

    if (configuration.quads.size() != Fw2051ScpQuadCount)
    {
        return "The SCP snapshot must contain exactly eight quads.";
    }

    for (std::size_t index = 0; index < configuration.quads.size(); ++index)
    {
        if (configuration.quads[index].quad != index)
        {
            return "The SCP snapshot quads are missing, duplicated, or "
                   "out of order.";
        }
    }

    return {};
}

ScpProfileSerializationResult SerializeFw2051ScpProfile(
    const Fw2051ScpConfigurationSnapshot& configuration)
{
    ScpProfileSerializationResult result;
    const std::string validationError =
        ValidateFw2051ScpConfiguration(configuration);
    if (!validationError.empty())
    {
        result.message = validationError;
        return result;
    }

    std::ostringstream output;
    output << ProfileMagic << ' ' << Fw2051ScpProfileFormatVersion << '\n';
    output << "GLOBAL " << configuration.baseAddress
           << ' ' << configuration.hardwareId
           << ' ' << configuration.firmwareRevision
           << ' ' << configuration.irqLevel
           << ' ' << configuration.outputFormat << '\n';
    for (const auto& quad : configuration.quads)
    {
        WriteQuad(output, quad);
    }
    output << "CHECKSUM " << ConfigurationChecksum(configuration) << '\n';
    output << "END\n";

    if (!output)
    {
        result.message = "Writing the SCP profile text failed.";
        return result;
    }

    result.success = true;
    result.text = output.str();
    return result;
}

ScpProfileParseResult ParseFw2051ScpProfile(const std::string& text)
{
    ScpProfileParseResult result;
    std::istringstream input(text);
    ScpProfile profile;
    std::string error;

    if (!ExpectToken(input, ProfileMagic, error)
        || !ReadUnsigned(
            input, profile.formatVersion, "profile version", error))
    {
        result.message = "Invalid SCP profile: " + error;
        return result;
    }

    if (profile.formatVersion != Fw2051ScpProfileFormatVersion)
    {
        result.message = "Unsupported SCP profile version "
            + std::to_string(profile.formatVersion) + ".";
        return result;
    }

    auto& configuration = profile.configuration;
    configuration.state = ScpConfigurationState::Complete;
    configuration.selectorParkedAtQuadZero = true;
    if (!ExpectToken(input, "GLOBAL", error)
        || !ReadUnsigned(input, configuration.baseAddress, "VME base", error)
        || !ReadUnsigned(input, configuration.hardwareId, "hardware ID", error)
        || !ReadUnsigned(
            input,
            configuration.firmwareRevision,
            "firmware revision",
            error)
        || !ReadUnsigned(input, configuration.irqLevel, "IRQ level", error)
        || !ReadUnsigned(
            input, configuration.outputFormat, "output format", error))
    {
        result.message = "Invalid SCP profile: " + error;
        return result;
    }

    configuration.quads.reserve(Fw2051ScpQuadCount);
    for (std::size_t index = 0; index < Fw2051ScpQuadCount; ++index)
    {
        Fw2051ScpQuadConfiguration quad;
        if (!ReadQuad(input, quad, error))
        {
            result.message = "Invalid SCP profile: " + error;
            return result;
        }
        configuration.quads.push_back(std::move(quad));
    }

    std::uint64_t storedChecksum = 0U;
    if (!ExpectToken(input, "CHECKSUM", error)
        || !ReadUnsigned(input, storedChecksum, "profile checksum", error))
    {
        result.message = "Invalid SCP profile: " + error;
        return result;
    }

    if (storedChecksum != ConfigurationChecksum(configuration))
    {
        result.message =
            "Invalid SCP profile: checksum mismatch; the file is damaged "
            "or was modified outside the workbench.";
        return result;
    }

    if (!ExpectToken(input, "END", error))
    {
        result.message = "Invalid SCP profile: " + error;
        return result;
    }

    std::string trailingToken;
    if (input >> trailingToken)
    {
        result.message = "Invalid SCP profile: unexpected trailing token '"
            + trailingToken + "'.";
        return result;
    }

    const std::string validationError =
        ValidateFw2051ScpConfiguration(configuration);
    if (!validationError.empty())
    {
        result.message = "Invalid SCP profile: " + validationError;
        return result;
    }

    configuration.message = "Loaded from SCP profile text.";
    result.success = true;
    result.message = configuration.message;
    result.profile = std::move(profile);
    return result;
}

ScpProfileSaveResult SaveFw2051ScpProfile(
    const Fw2051ScpConfigurationSnapshot& configuration,
    const std::string& path)
{
    ScpProfileSaveResult result;
    if (path.empty())
    {
        result.message = "The profile path is empty.";
        return result;
    }

    const auto serialized = SerializeFw2051ScpProfile(configuration);
    if (!serialized.success)
    {
        result.message = "Profile not saved: " + serialized.message;
        return result;
    }

    const std::string temporary = path + ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        result.message = "Could not open temporary profile '" + temporary
            + "' for writing.";
        return result;
    }

    output.write(serialized.text.data(),
                 static_cast<std::streamsize>(serialized.text.size()));
    output.flush();
    output.close();

    if (!output)
    {
        std::remove(temporary.c_str());
        result.message = "Writing the SCP profile failed.";
        return result;
    }

    if (std::rename(temporary.c_str(), path.c_str()) != 0)
    {
        const std::string renameError = std::strerror(errno);
        std::remove(temporary.c_str());
        result.message = "Could not install the completed SCP profile: "
            + renameError + '.';
        return result;
    }

    result.success = true;
    result.message = "Saved the read-only SCP profile to '" + path + "'.";
    return result;
}

ScpProfileLoadResult LoadFw2051ScpProfile(const std::string& path)
{
    ScpProfileLoadResult result;
    if (path.empty())
    {
        result.message = "The profile path is empty.";
        return result;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        result.message = "Could not open '" + path + "' for reading.";
        return result;
    }

    std::ostringstream contents;
    contents << input.rdbuf();
    if (input.bad())
    {
        result.message = "Could not read '" + path + "'.";
        return result;
    }

    auto parsed = ParseFw2051ScpProfile(contents.str());
    if (!parsed.success || !parsed.profile)
    {
        result.message = parsed.message;
        return result;
    }

    parsed.profile->configuration.message =
        "Loaded from local SCP profile '" + path + "'.";
    result.success = true;
    result.message = parsed.profile->configuration.message;
    result.profile = std::move(parsed.profile);
    return result;
}

ScpConfigurationComparison CompareFw2051ScpConfiguration(
    const ScpProfile& profile,
    const Fw2051ScpConfigurationSnapshot& liveConfiguration)
{
    ScpConfigurationComparison comparison;
    if (profile.formatVersion != Fw2051ScpProfileFormatVersion)
    {
        comparison.message = "The loaded SCP profile version is unsupported.";
        return comparison;
    }

    std::string validationError =
        ValidateFw2051ScpConfiguration(profile.configuration);
    if (!validationError.empty())
    {
        comparison.message = "The loaded profile is invalid: "
            + validationError;
        return comparison;
    }

    validationError = ValidateFw2051ScpConfiguration(liveConfiguration);
    if (!validationError.empty())
    {
        comparison.message = "The live snapshot is not comparable: "
            + validationError;
        return comparison;
    }

    comparison.comparable = true;
    const auto& expected = profile.configuration;
    AddDifference(comparison,
                  -1,
                  false,
                  0U,
                  "VME base",
                  expected.baseAddress,
                  liveConfiguration.baseAddress,
                  true);
    AddDifference(comparison,
                  -1,
                  true,
                  0x6008U,
                  "Hardware ID",
                  expected.hardwareId,
                  liveConfiguration.hardwareId,
                  true);
    AddDifference(comparison,
                  -1,
                  true,
                  0x600EU,
                  "Firmware revision",
                  expected.firmwareRevision,
                  liveConfiguration.firmwareRevision,
                  true);
    AddDifference(comparison,
                  -1,
                  true,
                  0x6010U,
                  "IRQ level",
                  expected.irqLevel,
                  liveConfiguration.irqLevel);
    AddDifference(comparison,
                  -1,
                  true,
                  0x6044U,
                  "Output format",
                  expected.outputFormat,
                  liveConfiguration.outputFormat,
                  true);

    for (std::size_t index = 0; index < expected.quads.size(); ++index)
    {
        const auto& profileQuad = expected.quads[index];
        const auto& liveQuad = liveConfiguration.quads[index];
        const int quad = static_cast<int>(index);

        for (const auto& definition : Fw2051ScpSettingRegistry)
        {
            const auto profileValue = Fw2051ScpQuadRegisterValue(
                profileQuad, definition.registerOffset);
            const auto liveValue = Fw2051ScpQuadRegisterValue(
                liveQuad, definition.registerOffset);
            if (!profileValue || !liveValue)
            {
                comparison.comparable = false;
                comparison.message =
                    "The FW2051 registry does not map every profile value.";
                comparison.valuesCompared = 0U;
                comparison.differences.clear();
                return comparison;
            }

            AddDifference(
                comparison,
                quad,
                true,
                definition.registerOffset,
                definition.name,
                *profileValue,
                *liveValue,
                definition.registerOffset == 0x614AU);
        }
    }

    comparison.message = comparison.differences.empty()
        ? "Live hardware matches all "
            + std::to_string(comparison.valuesCompared)
            + " values in the loaded SCP profile."
        : std::to_string(comparison.differences.size()) + " of "
            + std::to_string(comparison.valuesCompared)
            + " captured SCP values differ from the loaded profile.";
    return comparison;
}

} // namespace fidget
