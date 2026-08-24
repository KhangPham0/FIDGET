#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "nlohmann/json.hpp"

static_assert(NLOHMANN_JSON_VERSION_MAJOR == 3, "JSON major version changed");
static_assert(NLOHMANN_JSON_VERSION_MINOR == 12, "JSON minor version changed");
static_assert(NLOHMANN_JSON_VERSION_PATCH == 0, "JSON patch version changed");

TEST_CASE("the vendored JSON interface target is usable")
{
    const nlohmann::json value = nlohmann::json::object();
    CHECK(value.empty());
}
