#ifndef FIDGET_CORE_SCP_TRANSACTION_PLAN_H
#define FIDGET_CORE_SCP_TRANSACTION_PLAN_H

#include "core/ScpProfile.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fidget {

struct ScpProfileApplicationStep
{
    int quad = -1;
    std::uint16_t registerOffset = 0;
    std::string settingName;
    std::uint16_t expectedValue = 0;
    std::uint16_t profileValue = 0;
    bool displayHexadecimal = false;
};

struct ScpProfileApplicationRequest
{
    Fw2051ScpConfigurationSnapshot expectedLiveConfiguration;
    std::size_t valuesCompared = 0;
    std::size_t configurationDifferences = 0;
    std::vector<ScpProfileApplicationStep> steps;
};

struct ScpProfileApplicationPlan
{
    bool success = false;
    std::string message;
    ScpProfileApplicationRequest request;
};

[[nodiscard]] ScpProfileApplicationPlan PlanFw2051ScpProfileApplication(
    const ScpProfile& profile,
    const Fw2051ScpConfigurationSnapshot& liveConfiguration);

} // namespace fidget

#endif
