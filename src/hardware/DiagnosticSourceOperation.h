#ifndef FIDGET_HARDWARE_DIAGNOSTIC_SOURCE_OPERATION_H
#define FIDGET_HARDWARE_DIAGNOSTIC_SOURCE_OPERATION_H

#include "core/Acquisition.h"
#include "hardware/DiagnosticAcquisitionOperation.h"
#include "hardware/Transport.h"

#include <atomic>
#include <cstdint>

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
    const std::atomic<bool>& cancellationRequested);

} // namespace fidget

#endif
