#ifndef FIDGET_HARDWARE_TARGET_PROBE_OPERATION_H
#define FIDGET_HARDWARE_TARGET_PROBE_OPERATION_H

#include "core/ScpConfiguration.h"
#include "core/TargetModuleAddress.h"
#include "core/TunerTarget.h"
#include "hardware/TransportFactory.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace fidget {

inline constexpr std::uint32_t TargetProbeExpectedMvlcHardwareId = 0x5008U;
inline constexpr std::uint32_t TargetProbeExpectedMvlcFirmware = 0x0046U;

// The probe always performs these read-only checks in this documented order:
// MVLC identity, MVLC firmware, MVLC DAQ mode, target identity, target
// firmware, then target acquisition control.
inline constexpr std::array<std::uint16_t, 3U>
    TargetProbeMvlcRegisterOrder{{0x6008U, 0x600EU, 0x1300U}};
inline constexpr std::array<std::uint16_t, 3U>
    TargetProbeMdppRegisterOrder{{
        0x6008U,
        0x600EU,
        Fw2051AcquisitionControlRegister,
    }};

struct TargetProbeRequest
{
    TransportEndpointRequest endpoint;
    TargetModuleAddress targetAddress;
};

// This operation never calls WriteVmeD16 or WriteLocalRegisters. Target reads
// use ReadVmeD16, whose accepted immediate-stack plumbing transiently writes
// stack memory and stack execution controls but performs no VME-bus write.
[[nodiscard]] TargetProbeResult RunTargetProbe(
    ITransportFactory& transportFactory,
    const TargetProbeRequest& request,
    const std::atomic<bool>& cancellationRequested);

} // namespace fidget

#endif
