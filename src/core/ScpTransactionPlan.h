#ifndef FIDGET_CORE_SCP_TRANSACTION_PLAN_H
#define FIDGET_CORE_SCP_TRANSACTION_PLAN_H

#include "core/ScpProfile.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fidget {

inline constexpr std::array<std::uint16_t, 13U>
    Fw2051ScpIndependentRegisterOrder{
        0x6112U, 0x6114U, 0x6116U, 0x6118U, 0x611AU,
        0x611CU, 0x611EU, 0x6120U, 0x6122U, 0x6126U,
        0x6128U, 0x612AU, 0x614AU,
    };

struct Fw2051ScpCoupledWriteOrderPlan
{
    bool success = false;
    std::string message;
    std::array<std::uint16_t, 2U> registerOffsets{};
};

// Orders one complete coupled transition using the same rule as profile
// application: move the constrained value first when it is valid against the
// current boundary; otherwise widen the boundary first. The caller must have
// established that the current pair is valid (or safely staged the boundary)
// before executing this order.
[[nodiscard]] Fw2051ScpCoupledWriteOrderPlan
PlanFw2051ScpCoupledWriteOrder(
    std::uint16_t constrainedRegister,
    std::uint16_t boundaryRegister,
    std::uint16_t currentBoundaryValue,
    std::uint16_t targetConstrainedValue,
    std::uint16_t targetBoundaryValue);

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

struct ScpStandaloneStartupPlan
{
    bool success = false;
    std::string message;
    std::size_t valuesCompared = 0;
    std::size_t configurationDifferences = 0;
    std::size_t startupContractDifferences = 0;
    std::size_t bankedDifferences = 0;
    ScpProfileApplicationRequest bankedApplication;
};

[[nodiscard]] ScpProfileApplicationPlan PlanFw2051ScpProfileApplication(
    const ScpProfile& profile,
    const Fw2051ScpConfigurationSnapshot& liveConfiguration);

[[nodiscard]] ScpStandaloneStartupPlan PlanFw2051ScpStandaloneStartup(
    const ScpProfile& profile,
    const Fw2051ScpConfigurationSnapshot& liveConfiguration);

} // namespace fidget

#endif
