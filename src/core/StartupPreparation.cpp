#include "core/StartupPreparation.h"

namespace fidget {

// This is the smallest deterministic module-wide contract proven by the
// current one-module IRQ/MBLT readout path. It intentionally excludes module
// identity, event marking/TDC choices, trigger I/O, timestamps, diagnostics,
// and every banked front-end setting.
const std::array<
    Fw2051StartupPreparationDefinition,
    Fw2051StartupPreparationRegisterCount>
    Fw2051StartupPreparationRegisterTable{{
        {0x6006U, "Fast MBLT", 1U},
        {0x6010U, "IRQ level", 1U},
        {0x6018U, "IRQ data threshold", 1U},
        {0x601AU, "Maximum transfer data", 1U},
        {0x601CU, "IRQ source", 0U},
        {0x601EU, "IRQ event threshold", 2U},
        {0x6036U, "Multi-event mode", 0x000BU},
        {0x6044U, "Output format", 0x0018U},
    }};

std::vector<Fw2051StartupPreparationMismatch>
FindFw2051StartupPreparationMismatches(
    const std::array<
        std::uint16_t,
        Fw2051StartupPreparationRegisterCount>& values)
{
    std::vector<Fw2051StartupPreparationMismatch> mismatches;
    mismatches.reserve(values.size());
    for (std::size_t index = 0U; index < values.size(); ++index)
    {
        const auto& definition = Fw2051StartupPreparationRegisterTable[index];
        if (values[index] == definition.targetValue)
        {
            continue;
        }
        mismatches.push_back({
            definition.registerOffset,
            definition.name,
            values[index],
            definition.targetValue,
        });
    }
    return mismatches;
}

} // namespace fidget
