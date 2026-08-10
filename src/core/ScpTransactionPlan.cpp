#include "core/ScpTransactionPlan.h"

#include "core/ScpRegistry.h"

#include <algorithm>
#include <array>
#include <limits>
#include <string>

namespace fidget {

ScpProfileApplicationPlan PlanFw2051ScpProfileApplication(
    const ScpProfile& profile,
    const Fw2051ScpConfigurationSnapshot& liveConfiguration)
{
    ScpProfileApplicationPlan plan;
    const auto comparison = CompareFw2051ScpConfiguration(
        profile, liveConfiguration);
    plan.request.expectedLiveConfiguration = liveConfiguration;
    plan.request.valuesCompared = comparison.valuesCompared;
    plan.request.configurationDifferences = comparison.differences.size();

    if (!comparison.comparable)
    {
        plan.message = comparison.message;
        return plan;
    }

    for (const auto& difference : comparison.differences)
    {
        if (difference.quad < 0 || !difference.hasRegister
            || difference.registerOffset
                > std::numeric_limits<std::uint16_t>::max()
            || FindFw2051ScpSetting(
                   static_cast<std::uint16_t>(difference.registerOffset))
                == nullptr)
        {
            plan.message =
                "Complete banked-profile application is blocked because "
                "the global setting '" + difference.setting
                + "' differs. Global initialization remains outside this "
                  "milestone.";
            return plan;
        }

        if (difference.profileValue
                > std::numeric_limits<std::uint16_t>::max()
            || difference.liveValue
                > std::numeric_limits<std::uint16_t>::max())
        {
            plan.message =
                "A profile difference is outside the D16 register range.";
            return plan;
        }

        const auto registerOffset = static_cast<std::uint16_t>(
            difference.registerOffset);
        const auto profileValue = static_cast<std::uint16_t>(
            difference.profileValue);
        const std::string validationError = ValidateFw2051ScpProfileValue(
            registerOffset, profileValue);
        if (!validationError.empty())
        {
            plan.message = "Quad " + std::to_string(difference.quad)
                + ": " + validationError;
            return plan;
        }
    }

    for (std::size_t index = 0U;
         index < profile.configuration.quads.size();
         ++index)
    {
        const auto& target = profile.configuration.quads[index];
        for (const auto& definition : Fw2051ScpSettingRegistry)
        {
            const auto value = Fw2051ScpQuadRegisterValue(
                target, definition.registerOffset);
            if (!value)
            {
                plan.message =
                    "The FW2051 registry does not map every profile value.";
                return plan;
            }

            const std::string validationError =
                ValidateFw2051ScpProfileValue(
                    definition.registerOffset, *value);
            if (!validationError.empty())
            {
                plan.message = "Quad " + std::to_string(index) + ": "
                    + validationError;
                return plan;
            }
        }

        if (target.timingFilter > target.shapingTime)
        {
            plan.message = "Quad " + std::to_string(index)
                + " has a profile timing filter greater than its shaping "
                  "time.";
            return plan;
        }
        if (target.preSamples >= target.totalSamples)
        {
            plan.message = "Quad " + std::to_string(index)
                + " has profile pre-samples at or after total samples.";
            return plan;
        }
    }

    const auto addDifference =
        [&comparison, &plan](const int quad,
                             const std::uint16_t registerOffset) {
            const auto found = std::find_if(
                comparison.differences.begin(),
                comparison.differences.end(),
                [quad, registerOffset](const auto& difference) {
                    return difference.quad == quad
                        && difference.hasRegister
                        && difference.registerOffset == registerOffset;
                });
            if (found == comparison.differences.end())
            {
                return;
            }

            plan.request.steps.push_back(ScpProfileApplicationStep{
                found->quad,
                registerOffset,
                found->setting,
                static_cast<std::uint16_t>(found->liveValue),
                static_cast<std::uint16_t>(found->profileValue),
                found->displayHexadecimal,
            });
        };

    constexpr std::array<std::uint16_t, 13> independentOrder{
        0x6112U, 0x6114U, 0x6116U, 0x6118U, 0x611AU,
        0x611CU, 0x611EU, 0x6120U, 0x6122U, 0x6126U,
        0x6128U, 0x612AU, 0x614AU,
    };

    for (std::size_t index = 0U;
         index < liveConfiguration.quads.size();
         ++index)
    {
        const int quad = static_cast<int>(index);
        const auto& live = liveConfiguration.quads[index];
        const auto& target = profile.configuration.quads[index];

        for (const auto registerOffset : independentOrder)
        {
            addDifference(quad, registerOffset);
        }

        // Widen a coupled boundary before moving its constrained value.
        // Otherwise, move the constrained value before shrinking the boundary.
        if (target.timingFilter > live.shapingTime)
        {
            addDifference(quad, 0x6124U);
            addDifference(quad, 0x6110U);
        }
        else
        {
            addDifference(quad, 0x6110U);
            addDifference(quad, 0x6124U);
        }

        if (target.preSamples >= live.totalSamples)
        {
            addDifference(quad, 0x6148U);
            addDifference(quad, 0x6146U);
        }
        else
        {
            addDifference(quad, 0x6146U);
            addDifference(quad, 0x6148U);
        }
    }

    if (plan.request.steps.size() != comparison.differences.size())
    {
        plan.message =
            "The banked-profile plan did not account for every captured "
            "difference; no hardware write is allowed.";
        plan.request.steps.clear();
        return plan;
    }

    plan.success = true;
    plan.message = plan.request.steps.empty()
        ? "The live banked SCP configuration already matches the profile."
        : "Prepared a guarded transaction for "
            + std::to_string(plan.request.steps.size())
            + " changed banked SCP value(s).";
    return plan;
}

ScpStandaloneStartupPlan PlanFw2051ScpStandaloneStartup(
    const ScpProfile& profile,
    const Fw2051ScpConfigurationSnapshot& liveConfiguration)
{
    ScpStandaloneStartupPlan plan;
    const auto comparison = CompareFw2051ScpConfiguration(
        profile, liveConfiguration);
    plan.valuesCompared = comparison.valuesCompared;
    plan.configurationDifferences = comparison.differences.size();

    if (!comparison.comparable)
    {
        plan.message = comparison.message;
        return plan;
    }

    constexpr std::uint16_t RequiredIrqLevel = 1U;
    constexpr std::uint16_t RequiredOutputFormat = 0x0018U;
    const auto& target = profile.configuration;

    if (target.hardwareId != Mdpp32HardwareId
        || target.firmwareRevision != Mdpp32ScpFirmwareRevisionFw2051
        || liveConfiguration.hardwareId != Mdpp32HardwareId
        || liveConfiguration.firmwareRevision
            != Mdpp32ScpFirmwareRevisionFw2051)
    {
        plan.message =
            "Deterministic startup is enabled only for the tested "
            "MDPP-32 hardware 0x5007 with SCP FW2051.";
        return plan;
    }

    if (target.irqLevel != RequiredIrqLevel
        || target.outputFormat != RequiredOutputFormat)
    {
        plan.message =
            "The saved profile is not a tuner-startup profile: it must "
            "record IRQ level 1 and sampled standard-streaming output "
            "format 0x0018.";
        return plan;
    }

    for (const auto& difference : comparison.differences)
    {
        if (difference.quad >= 0)
        {
            ++plan.bankedDifferences;
            continue;
        }

        if (difference.hasRegister
            && (difference.registerOffset == 0x6010U
                || difference.registerOffset == 0x6044U))
        {
            ++plan.startupContractDifferences;
            continue;
        }

        plan.message =
            "Deterministic startup is blocked by the global identity "
            "difference '" + difference.setting + "'.";
        return plan;
    }

    // Normalize only the two globals owned by the proven startup preparation.
    // The banked planner can then validate and order every 0x61xx difference.
    auto preparedConfiguration = liveConfiguration;
    preparedConfiguration.irqLevel = RequiredIrqLevel;
    preparedConfiguration.outputFormat = RequiredOutputFormat;
    const auto bankedPlan = PlanFw2051ScpProfileApplication(
        profile, preparedConfiguration);
    if (!bankedPlan.success)
    {
        plan.message = bankedPlan.message;
        return plan;
    }

    if (bankedPlan.request.steps.size() != plan.bankedDifferences)
    {
        plan.message =
            "The deterministic-startup planner did not account for every "
            "banked difference; no hardware operation is enabled.";
        return plan;
    }

    plan.bankedApplication = bankedPlan.request;
    plan.success = true;
    plan.message =
        "Prepared a deterministic tuner-startup recipe: "
        + std::to_string(plan.startupContractDifferences)
        + " profile-visible module-wide mismatch(es) are handled by the "
          "verified startup contract and "
        + std::to_string(plan.bankedDifferences)
        + " banked mismatch(es) by the saved SCP profile.";
    return plan;
}

} // namespace fidget
