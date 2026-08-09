#ifndef FIDGET_CORE_SCP_PROFILE_H
#define FIDGET_CORE_SCP_PROFILE_H

#include "core/ScpConfiguration.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fidget {

inline constexpr std::uint32_t Fw2051ScpProfileFormatVersion = 1U;

struct ScpProfile
{
    std::uint32_t formatVersion = Fw2051ScpProfileFormatVersion;
    Fw2051ScpConfigurationSnapshot configuration;
};

struct ScpProfileSerializationResult
{
    bool success = false;
    std::string message;
    std::string text;
};

struct ScpProfileParseResult
{
    bool success = false;
    std::string message;
    std::optional<ScpProfile> profile;
};

struct ScpProfileSaveResult
{
    bool success = false;
    std::string message;
};

struct ScpProfileLoadResult
{
    bool success = false;
    std::string message;
    std::optional<ScpProfile> profile;
};

struct ScpConfigurationDifference
{
    int quad = -1;
    bool hasRegister = true;
    std::uint32_t registerOffset = 0;
    std::string setting;
    std::uint32_t profileValue = 0;
    std::uint32_t liveValue = 0;
    bool displayHexadecimal = false;
};

struct ScpConfigurationComparison
{
    bool comparable = false;
    std::string message;
    std::size_t valuesCompared = 0;
    std::vector<ScpConfigurationDifference> differences;
};

[[nodiscard]] std::string ValidateFw2051ScpConfiguration(
    const Fw2051ScpConfigurationSnapshot& configuration);

[[nodiscard]] ScpProfileSerializationResult SerializeFw2051ScpProfile(
    const Fw2051ScpConfigurationSnapshot& configuration);

[[nodiscard]] ScpProfileParseResult ParseFw2051ScpProfile(
    const std::string& text);

[[nodiscard]] ScpProfileSaveResult SaveFw2051ScpProfile(
    const Fw2051ScpConfigurationSnapshot& configuration,
    const std::string& path);

[[nodiscard]] ScpProfileLoadResult LoadFw2051ScpProfile(
    const std::string& path);

[[nodiscard]] ScpConfigurationComparison CompareFw2051ScpConfiguration(
    const ScpProfile& profile,
    const Fw2051ScpConfigurationSnapshot& liveConfiguration);

} // namespace fidget

#endif
