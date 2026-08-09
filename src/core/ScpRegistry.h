#ifndef FIDGET_CORE_SCP_REGISTRY_H
#define FIDGET_CORE_SCP_REGISTRY_H

#include "core/ScpConfiguration.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>

namespace fidget {

inline constexpr std::uint16_t Fw2051ScpMaximumSampleCount = 488U;
inline constexpr std::uint16_t Fw2051ScpMinimumShapingTime = 8U;
inline constexpr std::uint16_t Fw2051ScpMaximumShapingTime = 1000U;
inline constexpr std::uint16_t Fw2051ScpSampleConfigurationAllowedMask =
    0x00C3U;

enum class Fw2051ScpValueRule
{
    Range,
    EvenRange,
    SampleConfiguration,
};

enum class Fw2051ScpDependencyRule
{
    None,
    NotAbove,
    NotBelow,
    LessThan,
    GreaterThan,
};

struct Fw2051ScpSettingDefinition
{
    std::uint16_t registerOffset;
    const char* name;
    std::uint16_t minimumValue;
    std::uint16_t maximumValue;
    Fw2051ScpValueRule valueRule;
    Fw2051ScpDependencyRule dependencyRule;
    std::uint16_t dependencyRegister;
    const char* dependencyName;
};

extern const std::array<
    Fw2051ScpSettingDefinition,
    Fw2051ScpBankedSettingCount> Fw2051ScpSettingRegistry;

[[nodiscard]] const Fw2051ScpSettingDefinition* FindFw2051ScpSetting(
    std::uint16_t registerOffset) noexcept;

[[nodiscard]] std::string ValidateFw2051ScpSettingValue(
    const Fw2051ScpSettingDefinition& definition,
    std::uint16_t value,
    const char* valueKind);

[[nodiscard]] std::string ValidateFw2051ScpProfileValue(
    std::uint16_t registerOffset,
    std::uint16_t value);

[[nodiscard]] bool Fw2051ScpDependencySatisfied(
    const Fw2051ScpSettingDefinition& definition,
    std::uint16_t value,
    std::uint16_t dependencyValue) noexcept;

[[nodiscard]] const char* Fw2051ScpDependencyRelation(
    Fw2051ScpDependencyRule rule) noexcept;

[[nodiscard]] std::optional<std::uint16_t> Fw2051ScpQuadRegisterValue(
    const Fw2051ScpQuadConfiguration& quad,
    std::uint16_t registerOffset);

[[nodiscard]] bool SetFw2051ScpQuadRegisterValue(
    Fw2051ScpQuadConfiguration& quad,
    std::uint16_t registerOffset,
    std::uint16_t value);

} // namespace fidget

#endif
