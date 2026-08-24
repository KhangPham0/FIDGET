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

// The staged Home checks use this documented order: Connect reads MVLC
// identity, MVLC firmware, and MVLC DAQ mode; Check then reads target identity,
// target firmware, and target acquisition control.
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

struct ControllerProbeRequest
{
    TransportEndpointRequest endpoint;
};

// Connect reads only the three MVLC-local registers above in one batch. It
// performs no local-register write and no VME-bus transaction.
[[nodiscard]] ControllerProbeResult RunControllerProbe(
    ITransportFactory& transportFactory,
    const ControllerProbeRequest& request,
    const std::atomic<bool>& cancellationRequested);

// Check reads only the three target registers above. It never calls
// WriteVmeD16 or WriteLocalRegisters. ReadVmeD16's accepted immediate-stack
// plumbing transiently writes stack memory and stack execution controls but
// performs no VME-bus write or module-setting write.
[[nodiscard]] TargetProbeResult RunTargetProbe(
    ITransportFactory& transportFactory,
    const TargetProbeRequest& request,
    const std::atomic<bool>& cancellationRequested);

} // namespace fidget

#endif
