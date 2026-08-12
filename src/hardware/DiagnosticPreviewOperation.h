#ifndef FIDGET_HARDWARE_DIAGNOSTIC_PREVIEW_OPERATION_H
#define FIDGET_HARDWARE_DIAGNOSTIC_PREVIEW_OPERATION_H

#include "core/Acquisition.h"
#include "hardware/DiagnosticAcquisitionOperation.h"
#include "hardware/Transport.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace fidget {

struct DiagnosticParameterPreviewRequest
{
    std::uint16_t selectedQuad = 0U;
    std::uint16_t registerOffset = 0U;
    std::uint16_t requestedValue = 0U;
};

[[nodiscard]] DiagnosticParameterPreviewResult ApplyDiagnosticParameterPreview(
    ICommandTransport& transport,
    DiagnosticAcquisitionPreparationResult& acquisitionSession,
    const DiagnosticParameterPreviewRequest& request,
    const std::string& recoveryJournalPath,
    const std::atomic<bool>& cancellationRequested);

[[nodiscard]] DiagnosticParameterPreviewResult
RestoreDiagnosticParameterPreview(
    ICommandTransport& transport,
    DiagnosticAcquisitionPreparationResult& acquisitionSession,
    const DiagnosticParameterPreviewResult& activePreview,
    const std::string& recoveryJournalPath,
    bool resumeAfterTransaction,
    bool automaticallyRestoredOnStop,
    const std::atomic<bool>& cancellationRequested);

} // namespace fidget

#endif
