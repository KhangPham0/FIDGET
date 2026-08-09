#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/ScpProfile.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
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
            static_cast<std::uint16_t>(2030U + quadIndex)};
        quad.gain = static_cast<std::uint16_t>(200U + quadIndex);
        quad.thresholds = {
            static_cast<std::uint16_t>(1000U + quadIndex * 10U),
            static_cast<std::uint16_t>(1001U + quadIndex * 10U),
            static_cast<std::uint16_t>(1002U + quadIndex * 10U),
            static_cast<std::uint16_t>(1003U + quadIndex * 10U)};
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

std::string ReadFixture(const char* name)
{
    const std::string path =
        std::string(FIDGET_TEST_FIXTURE_DIR) + '/' + name;
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

std::string MakeTemporaryPath(const char* suffix)
{
    const char* temporaryDirectory = std::getenv("TMPDIR");
    if (temporaryDirectory == nullptr || temporaryDirectory[0] == '\0')
    {
        temporaryDirectory = "/tmp";
    }

    const auto unique = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    return std::string(temporaryDirectory) + "/fidget_scp_profile_"
        + std::to_string(unique) + suffix;
}

} // namespace

TEST_CASE("an FW2051 profile round-trips through its string format")
{
    using namespace fidget;

    const auto configuration = MakeConfiguration();
    const auto serialized = SerializeFw2051ScpProfile(configuration);
    REQUIRE(serialized.success);

    const auto parsed = ParseFw2051ScpProfile(serialized.text);
    REQUIRE(parsed.success);
    REQUIRE(parsed.profile.has_value());

    const auto comparison = CompareFw2051ScpConfiguration(
        *parsed.profile, configuration);
    CHECK(comparison.comparable);
    CHECK(comparison.valuesCompared == 141U);
    CHECK(comparison.valuesCompared == Fw2051ScpConfigurationValueCount);
    CHECK(comparison.differences.empty());
}

TEST_CASE("the real-hardware FW2051 golden profile remains compatible")
{
    using namespace fidget;

    const auto parsed = ParseFw2051ScpProfile(
        ReadFixture("mdpp1_scp_profile.mwwscp"));
    REQUIRE(parsed.success);
    REQUIRE(parsed.profile.has_value());
    CHECK(parsed.profile->configuration.baseAddress == 285212672U);
    CHECK(parsed.profile->configuration.hardwareId == 20487U);
    CHECK(parsed.profile->configuration.firmwareRevision == 8273U);
    CHECK(parsed.profile->configuration.quads.size() == 8U);
    CHECK(parsed.profile->configuration.quads[4].thresholds[2] == 2600U);
}

TEST_CASE("profile differences retain complete source attribution")
{
    using namespace fidget;

    const auto configuration = MakeConfiguration();
    ScpProfile profile;
    profile.configuration = configuration;

    auto changed = configuration;
    changed.quads[5].thresholds[1] = 4321U;
    changed.quads[7].sampleConfiguration = 0x0000U;
    changed.outputFormat = 0x0008U;

    const auto comparison = CompareFw2051ScpConfiguration(profile, changed);
    REQUIRE(comparison.comparable);
    CHECK(comparison.valuesCompared == 141U);
    REQUIRE(comparison.differences.size() == 3U);

    const auto& output = comparison.differences[0];
    CHECK(output.quad == -1);
    CHECK(output.registerOffset == 0x6044U);
    CHECK(output.setting == "Output format");
    CHECK(output.profileValue == 0x0018U);
    CHECK(output.liveValue == 0x0008U);

    const auto& threshold = comparison.differences[1];
    CHECK(threshold.quad == 5);
    CHECK(threshold.registerOffset == 0x611EU);
    CHECK(threshold.setting == "Threshold ch 1");
    CHECK(threshold.profileValue == configuration.quads[5].thresholds[1]);
    CHECK(threshold.liveValue == 4321U);

    const auto& sampleConfiguration = comparison.differences[2];
    CHECK(sampleConfiguration.quad == 7);
    CHECK(sampleConfiguration.registerOffset == 0x614AU);
    CHECK(sampleConfiguration.setting == "Sample configuration");
    CHECK(sampleConfiguration.profileValue == 0x0003U);
    CHECK(sampleConfiguration.liveValue == 0x0000U);
    CHECK(sampleConfiguration.displayHexadecimal);
}

TEST_CASE("the FW2051 profile format preserves every rejection boundary")
{
    using namespace fidget;

    const auto configuration = MakeConfiguration();
    const auto serialized = SerializeFw2051ScpProfile(configuration);
    REQUIRE(serialized.success);

    std::string unsupportedVersion = serialized.text;
    const auto version = unsupportedVersion.find("MWW_SCP_PROFILE 1");
    REQUIRE(version != std::string::npos);
    unsupportedVersion[version + 16U] = '2';
    const auto versionFailure = ParseFw2051ScpProfile(unsupportedVersion);
    CHECK_FALSE(versionFailure.success);
    CHECK(versionFailure.message == "Unsupported SCP profile version 2.");

    std::string tampered = serialized.text;
    const auto valuePosition = tampered.find("QUAD 0 8 ");
    REQUIRE(valuePosition != std::string::npos);
    tampered[valuePosition + 7U] = '9';
    const auto checksumFailure = ParseFw2051ScpProfile(tampered);
    CHECK_FALSE(checksumFailure.success);
    CHECK(checksumFailure.message.find("checksum mismatch")
          != std::string::npos);

    const auto trailing = ParseFw2051ScpProfile(
        serialized.text + "UNEXPECTED\n");
    CHECK_FALSE(trailing.success);
    CHECK(trailing.message.find("unexpected trailing token")
          != std::string::npos);

    auto missingQuad = configuration;
    missingQuad.quads.pop_back();
    const auto missingQuadFailure = SerializeFw2051ScpProfile(missingQuad);
    CHECK_FALSE(missingQuadFailure.success);
    CHECK(missingQuadFailure.message.find("exactly eight quads")
          != std::string::npos);

    auto duplicateQuad = configuration;
    duplicateQuad.quads[7].quad = 6U;
    const auto duplicateQuadFailure =
        SerializeFw2051ScpProfile(duplicateQuad);
    CHECK_FALSE(duplicateQuadFailure.success);
    CHECK(duplicateQuadFailure.message.find("missing, duplicated")
          != std::string::npos);

    auto nonMdpp = configuration;
    nonMdpp.hardwareId = 0x1234U;
    const auto nonMdppFailure = SerializeFw2051ScpProfile(nonMdpp);
    CHECK_FALSE(nonMdppFailure.success);
    CHECK(nonMdppFailure.message.find("MDPP-32") != std::string::npos);

    auto nonScp = configuration;
    nonScp.firmwareRevision = 0x1051U;
    const auto nonScpFailure = SerializeFw2051ScpProfile(nonScp);
    CHECK_FALSE(nonScpFailure.success);
    CHECK(nonScpFailure.message ==
          "The snapshot does not report SCP firmware.");

    auto untestedScp = configuration;
    untestedScp.firmwareRevision = 0x2052U;
    const auto untestedScpFailure = SerializeFw2051ScpProfile(untestedScp);
    CHECK_FALSE(untestedScpFailure.success);
    CHECK(untestedScpFailure.message.find("supported SCP FW2051")
          != std::string::npos);

    const auto malformed = ParseFw2051ScpProfile(
        "MWW_SCP_PROFILE 1\nGLOBAL 1 2 3\n");
    CHECK_FALSE(malformed.success);
    CHECK(malformed.message.find("Invalid SCP profile")
          != std::string::npos);
}

TEST_CASE("the thin profile file wrapper saves and loads atomically")
{
    using namespace fidget;

    const auto configuration = MakeConfiguration();
    const std::string path = MakeTemporaryPath(".mwwscp");
    const std::string temporary = path + ".tmp";

    const auto saved = SaveFw2051ScpProfile(configuration, path);
    REQUIRE(saved.success);
    CHECK(std::ifstream(temporary).fail());

    const auto loaded = LoadFw2051ScpProfile(path);
    REQUIRE(loaded.success);
    REQUIRE(loaded.profile.has_value());
    const auto comparison = CompareFw2051ScpConfiguration(
        *loaded.profile, configuration);
    CHECK(comparison.comparable);
    CHECK(comparison.valuesCompared == 141U);
    CHECK(comparison.differences.empty());

    std::remove(path.c_str());
    std::remove(temporary.c_str());
}
