#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/MvmeExport.h"
#include "core/ScpProfile.h"

#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string ReadFixture(const std::string& name)
{
    std::ifstream input(
        std::string(FIDGET_TEST_FIXTURE_DIR) + '/' + name,
        std::ios::binary);
    REQUIRE(input.good());
    std::ostringstream contents;
    contents << input.rdbuf();
    REQUIRE_FALSE(input.bad());
    return contents.str();
}

fidget::ScpProfile LoadFixtureProfile()
{
    const auto parsed = fidget::ParseFw2051ScpProfile(
        ReadFixture("mdpp1_scp_profile.mwwscp"));
    REQUIRE(parsed.success);
    REQUIRE(parsed.profile.has_value());
    return *parsed.profile;
}

} // namespace

TEST_CASE("FW2051 MVME export matches the reviewed golden script")
{
    using namespace fidget;

    const auto profile = LoadFixtureProfile();
    const auto generated = GenerateFw2051MvmeScript(
        profile, "2026-08-17T12:00:00Z");

    REQUIRE(generated.success);
    CHECK(generated.valueCount == 141U);
    CHECK(generated.sourceProfileChecksum
          == ComputeFw2051ScpProfileChecksum(profile.configuration));
    CHECK(generated.sourceProfileChecksum == 17470103622431138743ULL);
    CHECK(generated.text == ReadFixture("mdpp1_scp_profile.mvme"));
}

TEST_CASE("FW2051 MVME export rejects invalid register values")
{
    using namespace fidget;

    auto profile = LoadFixtureProfile();
    profile.configuration.quads[0].totalSamples = 399U;
    const auto generated = GenerateFw2051MvmeScript(
        profile, "2026-08-17T12:00:00Z");

    CHECK_FALSE(generated.success);
    CHECK(generated.text.empty());
    CHECK(generated.message.find("even number of samples")
          != std::string::npos);
}

TEST_CASE("FW2051 MVME export rejects invalid dependencies and snapshots")
{
    using namespace fidget;

    auto profile = LoadFixtureProfile();
    profile.configuration.quads[0].preSamples = 400U;
    auto generated = GenerateFw2051MvmeScript(
        profile, "2026-08-17T12:00:00Z");
    CHECK_FALSE(generated.success);
    CHECK(generated.message.find("dependency") != std::string::npos);

    profile = LoadFixtureProfile();
    profile.configuration.state = ScpConfigurationState::Failed;
    generated = GenerateFw2051MvmeScript(
        profile, "2026-08-17T12:00:00Z");
    CHECK_FALSE(generated.success);
    CHECK(generated.message.find("not complete") != std::string::npos);
}
