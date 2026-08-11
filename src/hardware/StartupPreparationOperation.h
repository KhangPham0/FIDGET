#ifndef FIDGET_HARDWARE_STARTUP_PREPARATION_OPERATION_H
#define FIDGET_HARDWARE_STARTUP_PREPARATION_OPERATION_H

#include "core/StartupAudit.h"
#include "core/StartupPreparation.h"
#include "hardware/ScpCaptureOperation.h"
#include "hardware/Transport.h"

#include <atomic>
#include <cstdint>

namespace fidget {

[[nodiscard]] StartupPreparationResult PrepareFw2051ModuleForStartup(
    ICommandTransport& transport,
    std::uint32_t baseAddress,
    const StartupAuditResult& audit,
    const std::atomic<bool>& cancellationRequested,
    const ScpCaptureOwnershipGate& ownershipGate);

} // namespace fidget

#endif
