#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/TargetModuleAddress.h"
#include "core/TunerTarget.h"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace {

void CheckAddress(
    const std::string_view text,
    const std::uint32_t expectedFull,
    const std::uint16_t expectedShorthand)
{
    const auto parsed = fidget::ParseTargetModuleAddress(text);
    INFO(parsed.message);
    REQUIRE(parsed.success);
    REQUIRE(parsed.address.has_value());
    CHECK(parsed.address->FullA32Value() == expectedFull);
    CHECK(parsed.address->MvmeShorthandValue() == expectedShorthand);
}

} // namespace

TEST_CASE("MVME shorthand and full A32 forms normalize equivalently")
{
    using namespace fidget;

    const std::array<std::string_view, 4U> equivalent{{
        "0x1100",
        "1100",
        "0x11000000",
        "11000000",
    }};

    const auto expected = ParseTargetModuleAddress(equivalent.front());
    REQUIRE(expected.success);
    REQUIRE(expected.address.has_value());
    for (const auto text : equivalent)
    {
        const auto parsed = ParseTargetModuleAddress(text);
        INFO(text);
        INFO(parsed.message);
        REQUIRE(parsed.success);
        REQUIRE(parsed.address.has_value());
        CHECK(*parsed.address == *expected.address);
        CHECK(parsed.address->FullA32Value() == 0x11000000U);
    }
}

TEST_CASE("target-module addresses accept surrounding whitespace")
{
    CheckAddress("  \t0X1100\r\n", 0x11000000U, 0x1100U);
    CheckAddress("\n 11000000 \v", 0x11000000U, 0x1100U);
}

TEST_CASE("zero shorthand is a present normalized address")
{
    CheckAddress("0x0000", 0x00000000U, 0x0000U);
    CheckAddress("0000", 0x00000000U, 0x0000U);
}

TEST_CASE("the full shorthand range normalizes without overflow")
{
    CheckAddress("ffff", 0xFFFF0000U, 0xFFFFU);
    CheckAddress("0x00010000", 0x00010000U, 0x0001U);
}

TEST_CASE("target-module address overflow is rejected")
{
    const std::array<std::string_view, 2U> overflowing{{
        "0x100000000",
        "fffffffffffffffff",
    }};
    for (const auto text : overflowing)
    {
        const auto parsed = fidget::ParseTargetModuleAddress(text);
        INFO(text);
        CHECK_FALSE(parsed.success);
        CHECK_FALSE(parsed.address.has_value());
        CHECK(parsed.message.find("A32 range") != std::string::npos);
    }
}

TEST_CASE("misaligned full A32 addresses are rejected")
{
    const std::array<std::string_view, 3U> misaligned{{
        "0x00010001",
        "0x11000001",
        "ffffffff",
    }};
    for (const auto text : misaligned)
    {
        const auto parsed = fidget::ParseTargetModuleAddress(text);
        INFO(text);
        CHECK_FALSE(parsed.success);
        CHECK_FALSE(parsed.address.has_value());
        CHECK(parsed.message.find("64-KiB aligned") != std::string::npos);
    }
}

TEST_CASE("malformed target-module addresses fail closed")
{
    const std::array<std::string_view, 6U> malformed{{
        "",
        "   ",
        "0x",
        "0x11 00",
        "-1100",
        "target",
    }};
    for (const auto text : malformed)
    {
        const auto parsed = fidget::ParseTargetModuleAddress(text);
        INFO(text);
        CHECK_FALSE(parsed.success);
        CHECK_FALSE(parsed.address.has_value());
    }
}

TEST_CASE("Home target fields share one parse-level validation contract")
{
    using namespace fidget;

    TunerTargetInput input;
    input.mvlcHost = "controller.example";
    input.moduleAddress = "0x1100";
    auto validation = ValidateTunerTargetInput(input);
    REQUIRE(validation.success);
    CHECK(validation.endpointValid);
    CHECK(validation.moduleAddressValid);
    REQUIRE(validation.normalizedModuleAddress.has_value());
    CHECK(validation.normalizedModuleAddress->FullA32Value()
          == 0x11000000U);

    input.moduleAddress = "0x0000";
    validation = ValidateTunerTargetInput(input);
    REQUIRE(validation.success);
    REQUIRE(validation.normalizedModuleAddress.has_value());
    CHECK(validation.normalizedModuleAddress->FullA32Value() == 0U);

    input.endpointKind = TunerTargetEndpointKind::SshBridge;
    validation = ValidateTunerTargetInput(input);
    CHECK_FALSE(validation.success);
    CHECK_FALSE(validation.endpointValid);
    CHECK(validation.moduleAddressValid);

    input.sshDestination = "bridge-alias";
    input.remoteBridgeCommand = "/opt/fidget_bridge";
    CHECK(ValidateTunerTargetInput(input).success);

    input.mvlcHost = " \t ";
    validation = ValidateTunerTargetInput(input);
    CHECK_FALSE(validation.success);
    CHECK_FALSE(validation.endpointValid);

    input.mvlcHost = "controller.example";
    input.mvlcCommandPort = 0U;
    CHECK_FALSE(ValidateTunerTargetInput(input).success);
}

TEST_CASE("Home endpoint fields use the shared transport-safe contract")
{
    using namespace fidget;

    TunerTargetInput input;
    input.mvlcHost = "controller.example";
    input.moduleAddress = "0x1100";

    const auto checkEndpoint = [&](const bool expected) {
        const auto targetValidation = ValidateTunerTargetInput(input);
        const auto endpointValidation = ValidateControllerEndpoint({
            input.endpointKind,
            input.mvlcHost,
            input.mvlcCommandPort,
            input.sshDestination,
            input.remoteBridgeCommand,
        });
        CHECK(targetValidation.endpointValid == expected);
        CHECK(endpointValidation.success == expected);
        CHECK(targetValidation.endpointMessage == endpointValidation.message);
    };

    checkEndpoint(true);

    input.mvlcHost = "controller name";
    checkEndpoint(false);
    input.mvlcHost = std::string(256U, 'h');
    checkEndpoint(false);
    input.mvlcHost = "controller.example";
    input.mvlcCommandPort = 0U;
    checkEndpoint(false);
    input.mvlcCommandPort = 0xFFFFU;
    checkEndpoint(false);

    input.mvlcCommandPort = 32768U;
    input.endpointKind = TunerTargetEndpointKind::SshBridge;
    input.sshDestination = "bridge-alias";
    input.remoteBridgeCommand = "/opt/fidget_bridge";
    checkEndpoint(true);
    input.sshDestination = "bridge alias";
    checkEndpoint(false);
    input.sshDestination = "bridge-alias";
    input.remoteBridgeCommand = "fidget bridge";
    checkEndpoint(false);

    input.endpointKind = TunerTargetEndpointKind::DirectEthernet;
    checkEndpoint(true);
}
