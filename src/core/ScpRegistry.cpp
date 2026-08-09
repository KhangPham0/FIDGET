#include "core/ScpRegistry.h"

#include <algorithm>
#include <string>

namespace fidget {

const std::array<
    Fw2051ScpSettingDefinition,
    Fw2051ScpBankedSettingCount> Fw2051ScpSettingRegistry{{
    {0x6110U, "Timing filter", 1U, 125U,
     Fw2051ScpValueRule::Range,
     Fw2051ScpDependencyRule::NotAbove, 0x6124U, "Shaping time"},
    {0x6112U, "Pole-zero ch 0", 64U, 0xFFFFU,
     Fw2051ScpValueRule::Range,
     Fw2051ScpDependencyRule::None, 0U, ""},
    {0x6114U, "Pole-zero ch 1", 64U, 0xFFFFU,
     Fw2051ScpValueRule::Range,
     Fw2051ScpDependencyRule::None, 0U, ""},
    {0x6116U, "Pole-zero ch 2", 64U, 0xFFFFU,
     Fw2051ScpValueRule::Range,
     Fw2051ScpDependencyRule::None, 0U, ""},
    {0x6118U, "Pole-zero ch 3", 64U, 0xFFFFU,
     Fw2051ScpValueRule::Range,
     Fw2051ScpDependencyRule::None, 0U, ""},
    {0x611AU, "Gain", 100U, 20000U,
     Fw2051ScpValueRule::Range,
     Fw2051ScpDependencyRule::None, 0U, ""},
    {0x611CU, "Threshold ch 0", 0U, 0xFFFFU,
     Fw2051ScpValueRule::Range,
     Fw2051ScpDependencyRule::None, 0U, ""},
    {0x611EU, "Threshold ch 1", 0U, 0xFFFFU,
     Fw2051ScpValueRule::Range,
     Fw2051ScpDependencyRule::None, 0U, ""},
    {0x6120U, "Threshold ch 2", 0U, 0xFFFFU,
     Fw2051ScpValueRule::Range,
     Fw2051ScpDependencyRule::None, 0U, ""},
    {0x6122U, "Threshold ch 3", 0U, 0xFFFFU,
     Fw2051ScpValueRule::Range,
     Fw2051ScpDependencyRule::None, 0U, ""},
    {0x6124U, "Shaping time", Fw2051ScpMinimumShapingTime,
     Fw2051ScpMaximumShapingTime,
     Fw2051ScpValueRule::Range,
     Fw2051ScpDependencyRule::NotBelow, 0x6110U, "Timing filter"},
    {0x6126U, "Baseline restorer", 0U, 3U,
     Fw2051ScpValueRule::Range,
     Fw2051ScpDependencyRule::None, 0U, ""},
    {0x6128U, "Reset time", 16U, 1000U,
     Fw2051ScpValueRule::Range,
     Fw2051ScpDependencyRule::None, 0U, ""},
    {0x612AU, "Signal rise time", 0U, 127U,
     Fw2051ScpValueRule::Range,
     Fw2051ScpDependencyRule::None, 0U, ""},
    {0x6146U, "Pre-samples", 0U, Fw2051ScpMaximumSampleCount,
     Fw2051ScpValueRule::Range,
     Fw2051ScpDependencyRule::LessThan, 0x6148U, "Total samples"},
    {0x6148U, "Total samples", 2U, Fw2051ScpMaximumSampleCount,
     Fw2051ScpValueRule::EvenRange,
     Fw2051ScpDependencyRule::GreaterThan, 0x6146U, "Pre-samples"},
    {0x614AU, "Sample configuration", 0U,
     Fw2051ScpSampleConfigurationAllowedMask,
     Fw2051ScpValueRule::SampleConfiguration,
     Fw2051ScpDependencyRule::None, 0U, ""},
}};

const Fw2051ScpSettingDefinition* FindFw2051ScpSetting(
    std::uint16_t registerOffset) noexcept
{
    const auto definition = std::find_if(
        Fw2051ScpSettingRegistry.begin(),
        Fw2051ScpSettingRegistry.end(),
        [registerOffset](const Fw2051ScpSettingDefinition& candidate) {
            return candidate.registerOffset == registerOffset;
        });
    return definition == Fw2051ScpSettingRegistry.end()
        ? nullptr
        : &*definition;
}

std::string ValidateFw2051ScpSettingValue(
    const Fw2051ScpSettingDefinition& definition,
    std::uint16_t value,
    const char* valueKind)
{
    const std::string label = std::string(valueKind) + definition.name;
    if (definition.valueRule == Fw2051ScpValueRule::SampleConfiguration
        && (value & static_cast<std::uint16_t>(
                        ~Fw2051ScpSampleConfigurationAllowedMask)) != 0U)
    {
        return "The " + label + " contains unsupported reserved bits.";
    }

    if (value < definition.minimumValue || value > definition.maximumValue)
    {
        return "The " + label + " must be between "
            + std::to_string(definition.minimumValue) + " and "
            + std::to_string(definition.maximumValue)
            + " register units.";
    }

    if (definition.valueRule == Fw2051ScpValueRule::EvenRange
        && value % 2U != 0U)
    {
        return "The " + label + " must be an even number of samples.";
    }

    return {};
}

std::string ValidateFw2051ScpProfileValue(std::uint16_t registerOffset,
                                          std::uint16_t value)
{
    const auto* definition = FindFw2051ScpSetting(registerOffset);
    if (definition == nullptr)
    {
        return "Retained SCP profile application is not available for this "
               "register.";
    }
    return ValidateFw2051ScpSettingValue(*definition, value, "profile ");
}

bool Fw2051ScpDependencySatisfied(
    const Fw2051ScpSettingDefinition& definition,
    std::uint16_t value,
    std::uint16_t dependencyValue) noexcept
{
    switch (definition.dependencyRule)
    {
    case Fw2051ScpDependencyRule::None:
        return true;
    case Fw2051ScpDependencyRule::NotAbove:
        return value <= dependencyValue;
    case Fw2051ScpDependencyRule::NotBelow:
        return value >= dependencyValue;
    case Fw2051ScpDependencyRule::LessThan:
        return value < dependencyValue;
    case Fw2051ScpDependencyRule::GreaterThan:
        return value > dependencyValue;
    }

    return false;
}

const char* Fw2051ScpDependencyRelation(
    Fw2051ScpDependencyRule rule) noexcept
{
    switch (rule)
    {
    case Fw2051ScpDependencyRule::None:
        return "satisfy";
    case Fw2051ScpDependencyRule::NotAbove:
        return "no greater than";
    case Fw2051ScpDependencyRule::NotBelow:
        return "no less than";
    case Fw2051ScpDependencyRule::LessThan:
        return "less than";
    case Fw2051ScpDependencyRule::GreaterThan:
        return "greater than";
    }

    return "satisfy";
}

std::optional<std::uint16_t> Fw2051ScpQuadRegisterValue(
    const Fw2051ScpQuadConfiguration& quad,
    std::uint16_t registerOffset)
{
    switch (registerOffset)
    {
    case 0x6110U:
        return quad.timingFilter;
    case 0x6112U:
    case 0x6114U:
    case 0x6116U:
    case 0x6118U:
        return quad.poleZero[(registerOffset - 0x6112U) / 2U];
    case 0x611AU:
        return quad.gain;
    case 0x611CU:
    case 0x611EU:
    case 0x6120U:
    case 0x6122U:
        return quad.thresholds[(registerOffset - 0x611CU) / 2U];
    case 0x6124U:
        return quad.shapingTime;
    case 0x6126U:
        return quad.baselineRestorer;
    case 0x6128U:
        return quad.resetTime;
    case 0x612AU:
        return quad.signalRiseTime;
    case 0x6146U:
        return quad.preSamples;
    case 0x6148U:
        return quad.totalSamples;
    case 0x614AU:
        return quad.sampleConfiguration;
    default:
        return std::nullopt;
    }
}

bool SetFw2051ScpQuadRegisterValue(Fw2051ScpQuadConfiguration& quad,
                                   std::uint16_t registerOffset,
                                   std::uint16_t value)
{
    switch (registerOffset)
    {
    case 0x6110U:
        quad.timingFilter = value;
        return true;
    case 0x6112U:
    case 0x6114U:
    case 0x6116U:
    case 0x6118U:
        quad.poleZero[(registerOffset - 0x6112U) / 2U] = value;
        return true;
    case 0x611AU:
        quad.gain = value;
        return true;
    case 0x611CU:
    case 0x611EU:
    case 0x6120U:
    case 0x6122U:
        quad.thresholds[(registerOffset - 0x611CU) / 2U] = value;
        return true;
    case 0x6124U:
        quad.shapingTime = value;
        return true;
    case 0x6126U:
        quad.baselineRestorer = value;
        return true;
    case 0x6128U:
        quad.resetTime = value;
        return true;
    case 0x612AU:
        quad.signalRiseTime = value;
        return true;
    case 0x6146U:
        quad.preSamples = value;
        return true;
    case 0x6148U:
        quad.totalSamples = value;
        return true;
    case 0x614AU:
        quad.sampleConfiguration = value;
        return true;
    default:
        return false;
    }
}

} // namespace fidget
