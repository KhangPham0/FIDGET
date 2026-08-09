#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/ScpRegistry.h"

#include <array>
#include <cstdint>
#include <string>

TEST_CASE("the FW2051 registry covers every banked snapshot register")
{
    using namespace fidget;

    constexpr std::array<std::uint16_t, 17> supportedRegisters{
        0x6110U, 0x6112U, 0x6114U, 0x6116U, 0x6118U, 0x611AU,
        0x611CU, 0x611EU, 0x6120U, 0x6122U, 0x6124U, 0x6126U,
        0x6128U, 0x612AU, 0x6146U, 0x6148U, 0x614AU};

    CHECK(Fw2051ScpSettingRegistry.size() == 17U);
    for (const auto registerOffset : supportedRegisters)
    {
        CHECK(FindFw2051ScpSetting(registerOffset) != nullptr);
    }

    constexpr std::array<std::uint16_t, 4> unsupportedRegisters{
        0x6008U, 0x6044U, 0x6100U, 0x612CU};
    for (const auto registerOffset : unsupportedRegisters)
    {
        CHECK(FindFw2051ScpSetting(registerOffset) == nullptr);
    }
}

TEST_CASE("the FW2051 registry preserves every proven value rejection")
{
    using namespace fidget;

    struct InvalidValue
    {
        std::uint16_t registerOffset;
        std::uint16_t profileValue;
        const char* messageFragment;
    };

    constexpr std::array<InvalidValue, 9> invalidValues{{
        {0x6110U, 0U, "between 1 and 125"},
        {0x6112U, 63U, "between 64 and 65535"},
        {0x611AU, 99U, "between 100 and 20000"},
        {0x6126U, 4U, "between 0 and 3"},
        {0x6128U, 15U, "between 16 and 1000"},
        {0x612AU, 128U, "between 0 and 127"},
        {0x6146U, 489U, "between 0 and 488"},
        {0x6148U, 3U, "even number of samples"},
        {0x614AU, 4U, "unsupported reserved bits"},
    }};

    for (const auto& invalid : invalidValues)
    {
        const std::string error = ValidateFw2051ScpProfileValue(
            invalid.registerOffset, invalid.profileValue);
        CHECK(error.find(invalid.messageFragment) != std::string::npos);
    }

    CHECK(ValidateFw2051ScpProfileValue(0x6110U, 1U).empty());
    CHECK(ValidateFw2051ScpProfileValue(0x6148U, 488U).empty());
    CHECK(ValidateFw2051ScpProfileValue(0x614AU, 0x00C3U).empty());
    CHECK(ValidateFw2051ScpProfileValue(0x612CU, 0U)
          == "Retained SCP profile application is not available for this "
             "register.");
}

TEST_CASE("the FW2051 dependency rules retain their partner relationships")
{
    using namespace fidget;

    const auto* timing = FindFw2051ScpSetting(0x6110U);
    const auto* shaping = FindFw2051ScpSetting(0x6124U);
    const auto* preSamples = FindFw2051ScpSetting(0x6146U);
    const auto* totalSamples = FindFw2051ScpSetting(0x6148U);
    REQUIRE(timing != nullptr);
    REQUIRE(shaping != nullptr);
    REQUIRE(preSamples != nullptr);
    REQUIRE(totalSamples != nullptr);

    CHECK(timing->dependencyRegister == 0x6124U);
    CHECK(std::string(timing->dependencyName) == "Shaping time");
    CHECK(Fw2051ScpDependencySatisfied(*timing, 100U, 100U));
    CHECK_FALSE(Fw2051ScpDependencySatisfied(*timing, 125U, 100U));
    CHECK(std::string(Fw2051ScpDependencyRelation(timing->dependencyRule))
          == "no greater than");

    CHECK(shaping->dependencyRegister == 0x6110U);
    CHECK(Fw2051ScpDependencySatisfied(*shaping, 200U, 200U));
    CHECK_FALSE(Fw2051ScpDependencySatisfied(*shaping, 160U, 200U));
    CHECK(std::string(Fw2051ScpDependencyRelation(shaping->dependencyRule))
          == "no less than");

    CHECK(preSamples->dependencyRegister == 0x6148U);
    CHECK(Fw2051ScpDependencySatisfied(*preSamples, 99U, 100U));
    CHECK_FALSE(Fw2051ScpDependencySatisfied(*preSamples, 100U, 100U));
    CHECK(std::string(
              Fw2051ScpDependencyRelation(preSamples->dependencyRule))
          == "less than");

    CHECK(totalSamples->dependencyRegister == 0x6146U);
    CHECK(Fw2051ScpDependencySatisfied(*totalSamples, 101U, 100U));
    CHECK_FALSE(Fw2051ScpDependencySatisfied(*totalSamples, 100U, 100U));
    CHECK(std::string(
              Fw2051ScpDependencyRelation(totalSamples->dependencyRule))
          == "greater than");
}

TEST_CASE("the FW2051 registry maps every setting to the snapshot structure")
{
    using namespace fidget;

    Fw2051ScpQuadConfiguration quad;
    std::uint16_t value = 100U;
    for (const auto& definition : Fw2051ScpSettingRegistry)
    {
        CHECK(SetFw2051ScpQuadRegisterValue(
            quad, definition.registerOffset, value));
        const auto stored = Fw2051ScpQuadRegisterValue(
            quad, definition.registerOffset);
        REQUIRE(stored.has_value());
        CHECK(*stored == value);
        ++value;
    }

    CHECK_FALSE(SetFw2051ScpQuadRegisterValue(quad, 0x612CU, 1U));
    CHECK_FALSE(Fw2051ScpQuadRegisterValue(quad, 0x612CU).has_value());
}
