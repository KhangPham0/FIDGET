#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/StartupPreparation.h"

#include <array>
#include <cstdint>

TEST_CASE("the FW2051 startup preparation contract is exact")
{
    using namespace fidget;

    const std::array<std::uint16_t, Fw2051StartupPreparationRegisterCount>
        expectedOffsets{{
            0x6006U,
            0x6010U,
            0x6018U,
            0x601AU,
            0x601CU,
            0x601EU,
            0x6036U,
            0x6044U,
        }};
    const std::array<std::uint16_t, Fw2051StartupPreparationRegisterCount>
        expectedValues{{1U, 1U, 1U, 1U, 0U, 2U, 0x000BU, 0x0018U}};

    for (std::size_t index = 0U; index < expectedOffsets.size(); ++index)
    {
        CHECK(Fw2051StartupPreparationRegisterTable[index].registerOffset
              == expectedOffsets[index]);
        CHECK(Fw2051StartupPreparationRegisterTable[index].targetValue
              == expectedValues[index]);
    }
}

TEST_CASE("startup preparation reports only mismatched contract values")
{
    using namespace fidget;

    std::array<std::uint16_t, Fw2051StartupPreparationRegisterCount> values{{
        1U,
        2U,
        1U,
        1U,
        3U,
        2U,
        0x000BU,
        0x0008U,
    }};
    const auto mismatches = FindFw2051StartupPreparationMismatches(values);

    REQUIRE(mismatches.size() == 3U);
    CHECK(mismatches[0].registerOffset == 0x6010U);
    CHECK(mismatches[0].currentValue == 2U);
    CHECK(mismatches[0].targetValue == 1U);
    CHECK(mismatches[1].registerOffset == 0x601CU);
    CHECK(mismatches[1].currentValue == 3U);
    CHECK(mismatches[1].targetValue == 0U);
    CHECK(mismatches[2].registerOffset == 0x6044U);
    CHECK(mismatches[2].currentValue == 0x0008U);
    CHECK(mismatches[2].targetValue == 0x0018U);

    for (std::size_t index = 0U; index < values.size(); ++index)
    {
        values[index] =
            Fw2051StartupPreparationRegisterTable[index].targetValue;
    }
    CHECK(FindFw2051StartupPreparationMismatches(values).empty());
}
