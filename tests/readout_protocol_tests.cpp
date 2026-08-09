#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/ReadoutProtocol.h"

#include <algorithm>
#include <array>
#include <cstdint>

TEST_CASE("an IRQ1 readout plan uses the official MVLC encoding")
{
    using namespace fidget;

    constexpr std::uint32_t Base = 0x11000000U;
    const auto result = MakeMvlcSingleMdppReadoutPlan(Base, 1U);
    REQUIRE(result.success);

    const auto& plan = result.plan;
    CHECK(plan.stackId == 1U);
    CHECK(plan.stackMemoryOffset == 0x0200U);
    CHECK(plan.stackOffsetRegister == 0x1204U);
    CHECK(plan.stackTriggerRegister == 0x1104U);
    CHECK(plan.triggerValue == 0x0040U);

    const std::array expected{
        MvlcLocalRegisterWrite{0x2200U, 0xF3010000U},
        MvlcLocalRegisterWrite{0x2204U, 0x1208FFFFU},
        MvlcLocalRegisterWrite{0x2208U, Base},
        MvlcLocalRegisterWrite{0x220CU, 0x23090001U},
        MvlcLocalRegisterWrite{0x2210U, Base + 0x6034U},
        MvlcLocalRegisterWrite{0x2214U, 1U},
        MvlcLocalRegisterWrite{0x2218U, 0xF4000000U},
    };

    REQUIRE(plan.stackUploadWrites.size() == expected.size());
    CHECK(std::equal(expected.begin(),
                     expected.end(),
                     plan.stackUploadWrites.begin()));
}

TEST_CASE("readout plans accept IRQ6 and reject invalid hardware settings")
{
    using namespace fidget;

    const auto irq6 = MakeMvlcSingleMdppReadoutPlan(0x33330000U, 6U);
    REQUIRE(irq6.success);
    CHECK(irq6.plan.triggerValue == 0x0045U);

    CHECK_FALSE(MakeMvlcSingleMdppReadoutPlan(0x11000001U, 1U).success);
    CHECK_FALSE(MakeMvlcSingleMdppReadoutPlan(0x11000000U, 0U).success);
    CHECK_FALSE(MakeMvlcSingleMdppReadoutPlan(0x11000000U, 7U).success);
}
