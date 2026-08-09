#ifndef FIDGET_CORE_READOUT_PROTOCOL_H
#define FIDGET_CORE_READOUT_PROTOCOL_H

#include "core/VmeProtocol.h"

#include <cstdint>
#include <string>
#include <vector>

namespace fidget {

struct MvlcSingleMdppReadoutPlan
{
    std::uint8_t stackId = 1;
    std::uint16_t stackMemoryOffset = 0x0200;
    std::uint16_t stackOffsetRegister = 0x1204;
    std::uint16_t stackTriggerRegister = 0x1104;
    std::uint16_t triggerValue = 0;
    std::vector<MvlcLocalRegisterWrite> stackUploadWrites;
};

struct MvlcReadoutPlanResult
{
    bool success = false;
    MvlcSingleMdppReadoutPlan plan;
    std::string error;
};

// Builds the smallest autonomous MVLC readout stack used by the diagnostic
// waveform proof: one MBLT64 FIFO read from one MDPP followed by its readout
// reset. The returned local writes upload the stack to stack slot 1, with
// output directed to the MVLC Ethernet data pipe.
[[nodiscard]] MvlcReadoutPlanResult MakeMvlcSingleMdppReadoutPlan(
    std::uint32_t mdppBaseAddress,
    std::uint16_t irqLevel);

} // namespace fidget

#endif
