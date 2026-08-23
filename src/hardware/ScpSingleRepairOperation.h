#ifndef FIDGET_HARDWARE_SCP_SINGLE_REPAIR_OPERATION_H
#define FIDGET_HARDWARE_SCP_SINGLE_REPAIR_OPERATION_H

#include "core/ScpTransactionResult.h"
#include "hardware/ScpCaptureOperation.h"
#include "hardware/Transport.h"

#include <atomic>
#include <chrono>
#include <functional>

namespace fidget {

using ScpSingleRepairDelay =
    std::function<void(std::chrono::microseconds)>;

[[nodiscard]] ScpSingleRepairResult RepairFw2051ScpProfileValue(
    ICommandTransport& transport,
    const ScpSingleRepairRequest& request,
    const std::atomic<bool>& cancellationRequested,
    const ScpCaptureOwnershipGate& ownershipGate,
    const ScpSingleRepairDelay& settleDelay = {});

} // namespace fidget

#endif
