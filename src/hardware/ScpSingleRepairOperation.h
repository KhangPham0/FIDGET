#ifndef FIDGET_HARDWARE_SCP_SINGLE_REPAIR_OPERATION_H
#define FIDGET_HARDWARE_SCP_SINGLE_REPAIR_OPERATION_H

#include "core/ScpTransactionResult.h"
#include "hardware/ScpCaptureOperation.h"
#include "hardware/Transport.h"

#include <atomic>

namespace fidget {

[[nodiscard]] ScpSingleRepairResult RepairFw2051ScpProfileValue(
    ICommandTransport& transport,
    const ScpSingleRepairRequest& request,
    const std::atomic<bool>& cancellationRequested,
    const ScpCaptureOwnershipGate& ownershipGate);

} // namespace fidget

#endif
