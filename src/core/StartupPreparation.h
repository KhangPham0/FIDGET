#ifndef FIDGET_CORE_STARTUP_PREPARATION_H
#define FIDGET_CORE_STARTUP_PREPARATION_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace fidget {

inline constexpr std::size_t Fw2051StartupPreparationRegisterCount = 8U;

struct Fw2051StartupPreparationDefinition
{
    std::uint16_t registerOffset;
    const char* name;
    std::uint16_t targetValue;
};

struct Fw2051StartupPreparationMismatch
{
    std::uint16_t registerOffset = 0U;
    const char* name = "";
    std::uint16_t currentValue = 0U;
    std::uint16_t targetValue = 0U;
};

extern const std::array<
    Fw2051StartupPreparationDefinition,
    Fw2051StartupPreparationRegisterCount>
    Fw2051StartupPreparationRegisterTable;

[[nodiscard]] std::vector<Fw2051StartupPreparationMismatch>
FindFw2051StartupPreparationMismatches(
    const std::array<
        std::uint16_t,
        Fw2051StartupPreparationRegisterCount>& values);

} // namespace fidget

#endif
