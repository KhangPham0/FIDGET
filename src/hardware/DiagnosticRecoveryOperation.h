#ifndef FIDGET_HARDWARE_DIAGNOSTIC_RECOVERY_OPERATION_H
#define FIDGET_HARDWARE_DIAGNOSTIC_RECOVERY_OPERATION_H

#include "core/RecoveryVerification.h"
#include "hardware/Transport.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace fidget {

struct DiagnosticOrphanRecoveryRequest
{
    TunerRecoveryRecord record;
    std::string recoveryJournalPath;
    std::uint16_t nextSuperReference = 0x5000U;
    std::uint32_t nextStackReference = 0x9E000001U;
};

[[nodiscard]] DiagnosticOrphanRecoveryResult RecoverDiagnosticOrphan(
    ICommandTransport& transport,
    const DiagnosticOrphanRecoveryRequest& request,
    const std::atomic<bool>& cancellationRequested);

} // namespace fidget

#endif
