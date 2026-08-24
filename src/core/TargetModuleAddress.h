#ifndef FIDGET_CORE_TARGET_MODULE_ADDRESS_H
#define FIDGET_CORE_TARGET_MODULE_ADDRESS_H

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace fidget {

struct TargetModuleAddressParseResult;

class TargetModuleAddress
{
public:
    [[nodiscard]] constexpr std::uint32_t FullA32Value() const noexcept
    {
        return fullA32Value_;
    }

    [[nodiscard]] constexpr std::uint16_t MvmeShorthandValue() const noexcept
    {
        return static_cast<std::uint16_t>(fullA32Value_ >> 16U);
    }

    friend constexpr bool operator==(
        TargetModuleAddress left,
        TargetModuleAddress right) noexcept
    {
        return left.fullA32Value_ == right.fullA32Value_;
    }

    friend constexpr bool operator!=(
        TargetModuleAddress left,
        TargetModuleAddress right) noexcept
    {
        return !(left == right);
    }

private:
    explicit constexpr TargetModuleAddress(
        const std::uint32_t fullA32Value) noexcept
        : fullA32Value_(fullA32Value)
    {
    }

    std::uint32_t fullA32Value_ = 0U;

    friend TargetModuleAddressParseResult ParseTargetModuleAddress(
        std::string_view text);
    friend TargetModuleAddressParseResult ParseFullTargetModuleAddress(
        std::uint32_t fullA32Value);
};

struct TargetModuleAddressParseResult
{
    bool success = false;
    std::string message;
    std::optional<TargetModuleAddress> address;
};

// Input is hexadecimal with an optional 0x prefix. Values through 0xFFFF are
// MVME shorthand and are shifted into the upper A32 word exactly once. Larger
// values must already be full 64-KiB-aligned A32 addresses.
[[nodiscard]] TargetModuleAddressParseResult ParseTargetModuleAddress(
    std::string_view text);

// Parses an already-expanded numeric A32 address. Unlike the user-facing text
// parser, this never interprets small values as MVME shorthand.
[[nodiscard]] TargetModuleAddressParseResult ParseFullTargetModuleAddress(
    std::uint32_t fullA32Value);

} // namespace fidget

#endif
