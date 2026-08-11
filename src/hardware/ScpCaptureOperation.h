#ifndef FIDGET_HARDWARE_SCP_CAPTURE_OPERATION_H
#define FIDGET_HARDWARE_SCP_CAPTURE_OPERATION_H

#include "core/ScpConfiguration.h"
#include "hardware/Transport.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

namespace fidget {

inline constexpr std::uint16_t Fw2051ScpSelectorRegister = 0x6100U;

enum class ScpCaptureGateStatus
{
    Allowed,
    CommunicationUnavailable,
    OwnershipLost,
    Cancelled,
};

struct ScpCaptureGateResult
{
    ScpCaptureGateStatus status =
        ScpCaptureGateStatus::CommunicationUnavailable;
    std::string message;
};

using ScpCaptureOwnershipGate =
    std::function<ScpCaptureGateResult(const std::string& operationName)>;

struct ScpCaptureOperationResult
{
    Fw2051ScpConfigurationSnapshot configuration;
    ScpCaptureGateStatus lastGateStatus = ScpCaptureGateStatus::Allowed;
};

[[nodiscard]] ScpCaptureOperationResult CaptureFw2051ScpConfiguration(
    ICommandTransport& transport,
    std::uint32_t baseAddress,
    const std::atomic<bool>& cancellationRequested,
    const ScpCaptureOwnershipGate& ownershipGate);

} // namespace fidget

#endif
