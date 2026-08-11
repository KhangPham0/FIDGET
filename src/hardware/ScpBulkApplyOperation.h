#ifndef FIDGET_HARDWARE_SCP_BULK_APPLY_OPERATION_H
#define FIDGET_HARDWARE_SCP_BULK_APPLY_OPERATION_H

#include "core/ScpTransactionResult.h"
#include "hardware/ScpCaptureOperation.h"
#include "hardware/Transport.h"

#include <atomic>

namespace fidget {

[[nodiscard]] ScpBulkApplyResult ApplyFw2051ScpProfile(
    ICommandTransport& transport,
    const ScpProfileApplicationRequest& request,
    const std::atomic<bool>& cancellationRequested,
    const ScpCaptureOwnershipGate& ownershipGate);

} // namespace fidget

#endif
