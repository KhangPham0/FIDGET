#ifndef FIDGET_HARDWARE_DETERMINISTIC_STARTUP_OPERATION_H
#define FIDGET_HARDWARE_DETERMINISTIC_STARTUP_OPERATION_H

#include "core/DeterministicStartup.h"
#include "hardware/ScpCaptureOperation.h"
#include "hardware/Transport.h"

#include <atomic>

namespace fidget {

[[nodiscard]] DeterministicStartupResult RunFw2051DeterministicStartup(
    ICommandTransport& transport,
    const DeterministicStartupRequest& request,
    const std::atomic<bool>& cancellationRequested,
    const ScpCaptureOwnershipGate& ownershipGate);

} // namespace fidget

#endif
