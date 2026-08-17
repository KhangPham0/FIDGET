#ifndef FIDGET_HARDWARE_DIAGNOSTIC_SOURCE_OPERATION_H
#define FIDGET_HARDWARE_DIAGNOSTIC_SOURCE_OPERATION_H

#include "core/Acquisition.h"
#include "hardware/DiagnosticAcquisitionOperation.h"
#include "hardware/Transport.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace fidget {

struct DiagnosticSourceChangeRequest
{
    std::uint16_t selectedQuad = 0U;
    std::uint8_t requestedSource = 0U;
};

[[nodiscard]] DiagnosticSourceChangeResult ChangeDiagnosticWaveformSource(
    ICommandTransport& transport,
    DiagnosticAcquisitionPreparationResult& acquisitionSession,
    const DiagnosticSourceChangeRequest& request,
    const std::string& recoveryJournalPath,
    const std::atomic<bool>& cancellationRequested);

[[nodiscard]] DiagnosticSourceChangeResult RestoreDiagnosticWaveformSource(
    ICommandTransport& transport,
    DiagnosticAcquisitionPreparationResult& acquisitionSession,
    const std::string& recoveryJournalPath,
    bool ownershipAlreadyVerifiedAndPaused,
    const std::atomic<bool>& cancellationRequested);

} // namespace fidget

#endif
