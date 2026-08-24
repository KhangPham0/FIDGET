#include "core/TargetModuleAddress.h"

#include <charconv>
#include <limits>
#include <system_error>

namespace fidget {
namespace {

bool IsAsciiWhitespace(const char character) noexcept
{
    switch (character)
    {
    case ' ':
    case '\t':
    case '\n':
    case '\r':
    case '\f':
    case '\v':
        return true;
    default:
        return false;
    }
}

std::string_view TrimAsciiWhitespace(std::string_view text) noexcept
{
    while (!text.empty() && IsAsciiWhitespace(text.front()))
        text.remove_prefix(1U);
    while (!text.empty() && IsAsciiWhitespace(text.back()))
        text.remove_suffix(1U);
    return text;
}

} // namespace

TargetModuleAddressParseResult ParseTargetModuleAddress(
    std::string_view text)
{
    TargetModuleAddressParseResult result;
    text = TrimAsciiWhitespace(text);
    if (text.empty())
    {
        result.message = "The target-module address is empty.";
        return result;
    }

    if (text.size() >= 2U
        && text[0] == '0'
        && (text[1] == 'x' || text[1] == 'X'))
    {
        text.remove_prefix(2U);
    }
    if (text.empty())
    {
        result.message = "The target-module address has no hexadecimal digits.";
        return result;
    }

    std::uint64_t parsed = 0U;
    const auto conversion = std::from_chars(
        text.data(), text.data() + text.size(), parsed, 16);
    if (conversion.ec == std::errc::result_out_of_range
        || parsed > std::numeric_limits<std::uint32_t>::max())
    {
        result.message = "The target-module address exceeds the A32 range.";
        return result;
    }
    if (conversion.ec != std::errc{}
        || conversion.ptr != text.data() + text.size())
    {
        result.message = "The target-module address is not valid hexadecimal.";
        return result;
    }

    std::uint32_t normalized = static_cast<std::uint32_t>(parsed);
    if (normalized <= 0xFFFFU)
    {
        normalized <<= 16U;
    }
    else if ((normalized & 0xFFFFU) != 0U)
    {
        result.message =
            "A full A32 target-module address must be 64-KiB aligned.";
        return result;
    }

    result.success = true;
    result.address = TargetModuleAddress(normalized);
    result.message = "Normalized the target-module address.";
    return result;
}

TargetModuleAddressParseResult ParseFullTargetModuleAddress(
    const std::uint32_t fullA32Value)
{
    TargetModuleAddressParseResult result;
    if ((fullA32Value & 0xFFFFU) != 0U)
    {
        result.message =
            "A full A32 target-module address must be 64-KiB aligned.";
        return result;
    }

    result.success = true;
    result.address = TargetModuleAddress(fullA32Value);
    result.message = "Accepted the full numeric target-module address.";
    return result;
}

} // namespace fidget
