#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/ScpTransactionPlan.h"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <string>

namespace {

fidget::Fw2051ScpConfigurationSnapshot MakeConfiguration()
{
    using namespace fidget;

    Fw2051ScpConfigurationSnapshot configuration;
    configuration.state = ScpConfigurationState::Complete;
    configuration.message = "test configuration";
    configuration.baseAddress = 0x11000000U;
    configuration.hardwareId = 0x5007U;
    configuration.firmwareRevision = 0x2051U;
    configuration.irqLevel = 1U;
    configuration.outputFormat = 0x18U;
    configuration.selectorParkedAtQuadZero = true;

    for (std::uint16_t quadIndex = 0U; quadIndex < 8U; ++quadIndex)
    {
        Fw2051ScpQuadConfiguration quad;
        quad.quad = quadIndex;
        quad.timingFilter = static_cast<std::uint16_t>(8U + quadIndex);
        quad.poleZero = {
            static_cast<std::uint16_t>(2000U + quadIndex),
            static_cast<std::uint16_t>(2010U + quadIndex),
            static_cast<std::uint16_t>(2020U + quadIndex),
            static_cast<std::uint16_t>(2030U + quadIndex),
        };
        quad.gain = static_cast<std::uint16_t>(200U + quadIndex);
        quad.thresholds = {
            static_cast<std::uint16_t>(1000U + quadIndex * 10U),
            static_cast<std::uint16_t>(1001U + quadIndex * 10U),
            static_cast<std::uint16_t>(1002U + quadIndex * 10U),
            static_cast<std::uint16_t>(1003U + quadIndex * 10U),
        };
        quad.shapingTime = static_cast<std::uint16_t>(160U + quadIndex);
        quad.baselineRestorer = 2U;
        quad.resetTime = 16U;
        quad.signalRiseTime = 4U;
        quad.preSamples = 50U;
        quad.totalSamples = 400U;
        quad.sampleConfiguration =
            quadIndex == 7U ? 0x0003U : 0x0000U;
        configuration.quads.push_back(quad);
    }
    return configuration;
}

} // namespace

TEST_CASE("the complete application plan orders coupled changes safely")
{
    using namespace fidget;

    ScpProfile profile;
    profile.configuration = MakeConfiguration();
    auto live = profile.configuration;
    auto& liveQuad = live.quads[7];
    liveQuad.gain = 510U;
    liveQuad.timingFilter = 10U;
    liveQuad.shapingTime = 12U;
    liveQuad.preSamples = 10U;
    liveQuad.totalSamples = 20U;

    const auto plan = PlanFw2051ScpProfileApplication(profile, live);
    INFO(plan.message);
    REQUIRE(plan.success);
    CHECK(plan.request.valuesCompared == 141U);
    CHECK(plan.request.configurationDifferences == 5U);
    REQUIRE(plan.request.steps.size() == 5U);

    const auto positionOf = [&plan](const std::uint16_t registerOffset) {
        const auto found = std::find_if(
            plan.request.steps.begin(),
            plan.request.steps.end(),
            [registerOffset](const auto& step) {
                return step.quad == 7
                    && step.registerOffset == registerOffset;
            });
        REQUIRE(found != plan.request.steps.end());
        return std::distance(plan.request.steps.begin(), found);
    };

    CHECK(positionOf(0x6124U) < positionOf(0x6110U));
    CHECK(positionOf(0x6148U) < positionOf(0x6146U));
}

TEST_CASE("complete application rejects global and invalid targets")
{
    using namespace fidget;

    ScpProfile profile;
    profile.configuration = MakeConfiguration();

    auto globalMismatch = profile.configuration;
    globalMismatch.outputFormat = 0x0008U;
    const auto globalPlan = PlanFw2051ScpProfileApplication(
        profile, globalMismatch);
    CHECK_FALSE(globalPlan.success);
    CHECK(globalPlan.request.steps.empty());
    CHECK(globalPlan.message.find("global setting") != std::string::npos);

    auto invalidProfile = profile;
    invalidProfile.configuration.quads[7].timingFilter = 100U;
    invalidProfile.configuration.quads[7].shapingTime = 80U;
    auto live = profile.configuration;
    live.quads[7].timingFilter = 90U;
    const auto invalidPlan = PlanFw2051ScpProfileApplication(
        invalidProfile, live);
    CHECK_FALSE(invalidPlan.success);
    CHECK(invalidPlan.request.steps.empty());
}

TEST_CASE("standalone startup partitions only its owned differences")
{
    using namespace fidget;

    ScpProfile profile;
    profile.configuration = MakeConfiguration();

    auto live = profile.configuration;
    live.irqLevel = 2U;
    live.outputFormat = 0x0008U;
    live.quads[7].sampleConfiguration = 0x0000U;

    const auto plan = PlanFw2051ScpStandaloneStartup(profile, live);
    INFO(plan.message);
    REQUIRE(plan.success);
    CHECK(plan.valuesCompared == 141U);
    CHECK(plan.configurationDifferences == 3U);
    CHECK(plan.startupContractDifferences == 2U);
    CHECK(plan.bankedDifferences == 1U);
    REQUIRE(plan.bankedApplication.steps.size() == 1U);
    CHECK(plan.bankedApplication.expectedLiveConfiguration.irqLevel == 1U);
    CHECK(plan.bankedApplication.expectedLiveConfiguration.outputFormat
          == 0x0018U);

    const auto& step = plan.bankedApplication.steps.front();
    CHECK(step.quad == 7);
    CHECK(step.registerOffset == 0x614AU);
    CHECK(step.expectedValue == 0x0000U);
    CHECK(step.profileValue == 0x0003U);
}

TEST_CASE("standalone startup rejects unsafe profiles and identity changes")
{
    using namespace fidget;

    ScpProfile profile;
    profile.configuration = MakeConfiguration();

    auto incompatibleProfile = profile;
    incompatibleProfile.configuration.outputFormat = 0x0008U;
    const auto incompatible = PlanFw2051ScpStandaloneStartup(
        incompatibleProfile, profile.configuration);
    CHECK_FALSE(incompatible.success);
    CHECK(incompatible.bankedApplication.steps.empty());

    auto wrongBase = profile.configuration;
    wrongBase.baseAddress = 0x33330000U;
    const auto identityMismatch = PlanFw2051ScpStandaloneStartup(
        profile, wrongBase);
    CHECK_FALSE(identityMismatch.success);
    CHECK(identityMismatch.message.find("VME base") != std::string::npos);

    auto untestedFirmware = profile.configuration;
    untestedFirmware.firmwareRevision = 0x2052U;
    auto matchingUntestedProfile = profile;
    matchingUntestedProfile.configuration.firmwareRevision = 0x2052U;
    const auto untested = PlanFw2051ScpStandaloneStartup(
        matchingUntestedProfile, untestedFirmware);
    CHECK_FALSE(untested.success);
    CHECK(untested.bankedApplication.steps.empty());
}

TEST_CASE("standalone startup plans no writes for an exact configuration")
{
    using namespace fidget;

    ScpProfile profile;
    profile.configuration = MakeConfiguration();

    const auto plan = PlanFw2051ScpStandaloneStartup(
        profile, profile.configuration);
    INFO(plan.message);
    CHECK(plan.success);
    CHECK(plan.configurationDifferences == 0U);
    CHECK(plan.startupContractDifferences == 0U);
    CHECK(plan.bankedDifferences == 0U);
    CHECK(plan.bankedApplication.steps.empty());
}
