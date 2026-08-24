#ifndef FIDGET_HARDWARE_SCP_CAPTURE_OPERATION_H
#define FIDGET_HARDWARE_SCP_CAPTURE_OPERATION_H

#include "core/ScpConfiguration.h"
#include "hardware/Transport.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace fidget {

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

enum class ScpCaptureCheckpointKind
{
    GlobalRegisterRead,
    SelectorSettled,
    BankedRegisterRead,
    SelectorParked,
};

struct ScpCaptureCheckpoint
{
    ScpCaptureCheckpointKind kind =
        ScpCaptureCheckpointKind::GlobalRegisterRead;
    std::optional<std::uint16_t> quad;
    std::uint16_t registerOffset = 0U;
};

using ScpCaptureDelay =
    std::function<void(std::chrono::microseconds)>;
using ScpCaptureCheckpointGate =
    std::function<bool(const ScpCaptureCheckpoint&)>;

struct ScpCaptureRuntime
{
    ScpCaptureDelay delay;
    ScpCaptureCheckpointGate checkpoint;
    // Existing ownership callers already gate before globals and quads 1-7.
    // Session preparation enables this additional immediate gate so no four
    // intervening global reads separate its DAQ check from selector quad 0.
    bool gateBeforeFirstSelector = false;
};

[[nodiscard]] ScpCaptureOperationResult CaptureFw2051ScpConfiguration(
    ICommandTransport& transport,
    std::uint32_t baseAddress,
    const std::atomic<bool>& cancellationRequested,
    const ScpCaptureOwnershipGate& ownershipGate);

// The runtime overload preserves the production operation while exposing the
// selector delay and completed-wire-step boundaries needed for exhaustive
// crash testing. A rejected checkpoint stops immediately and deliberately
// sends no cleanup write.
[[nodiscard]] ScpCaptureOperationResult CaptureFw2051ScpConfiguration(
    ICommandTransport& transport,
    std::uint32_t baseAddress,
    const std::atomic<bool>& cancellationRequested,
    const ScpCaptureOwnershipGate& ownershipGate,
    const ScpCaptureRuntime& runtime);

} // namespace fidget

#endif
