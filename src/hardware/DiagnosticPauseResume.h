#ifndef FIDGET_HARDWARE_DIAGNOSTIC_PAUSE_RESUME_H
#define FIDGET_HARDWARE_DIAGNOSTIC_PAUSE_RESUME_H

#include "hardware/Transport.h"

#include <atomic>
#include <cstdint>
#include <string>

namespace fidget {

struct DiagnosticPauseResult
{
    bool modulePaused = false;
    bool daqModePaused = false;
    std::string error;
};

struct DiagnosticResumeResult
{
    bool fifoResetSent = false;
    bool readoutResetSent = false;
    bool acquisitionResumed = false;
    bool daqModeReadbackValid = false;
    std::uint32_t daqModeReadback = 0U;
    bool daqModeResumed = false;
    std::string error;
};

[[nodiscard]] DiagnosticPauseResult PauseDiagnosticDataTaking(
    ICommandTransport& transport,
    std::uint32_t baseAddress,
    std::uint16_t& nextSuperReference,
    std::uint32_t& nextStackReference,
    const std::atomic<bool>& cancellationRequested);

[[nodiscard]] DiagnosticResumeResult ResumeDiagnosticDataTaking(
    ICommandTransport& transport,
    std::uint32_t baseAddress,
    std::uint16_t& nextSuperReference,
    std::uint32_t& nextStackReference,
    const std::atomic<bool>& cancellationRequested);

} // namespace fidget

#endif
