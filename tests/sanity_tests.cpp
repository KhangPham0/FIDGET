#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

// Proves the test harness builds and runs. Real tests arrive with the core
// wire formats in phase 3.
TEST_CASE("the test harness runs")
{
    CHECK(1 + 1 == 2);
}
