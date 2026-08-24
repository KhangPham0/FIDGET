#ifndef FIDGET_CORE_TUNER_TARGET_H
#define FIDGET_CORE_TUNER_TARGET_H

#include "core/TargetModuleAddress.h"
#include "core/TuningSessionState.h"

#include <cstdint>
#include <optional>
#include <string>

namespace fidget {

enum class TunerTargetEndpointKind
{
    DirectEthernet,
    SshBridge,
};

// Editable, project-independent target fields. SSH authentication is external
// to FIDGET, so this type deliberately has no password, key, or secret field.
struct TunerTargetInput
{
    TunerTargetEndpointKind endpointKind =
        TunerTargetEndpointKind::DirectEthernet;
    std::string mvlcHost;
    std::uint16_t mvlcCommandPort = 32768U;
    std::string moduleAddress;
    std::string sshDestination;
    std::string remoteBridgeCommand = "fidget_bridge";
};

// Parse-level validation for the editable Home fields. Hostnames and SSH
// destinations are intentionally treated as opaque, non-empty endpoint
// strings. The transport resolves IPv4 today; aliases and remote-side names
// remain valid inputs.
// The module address is normalized by the one shared parser used by hardware.
struct TunerTargetInputValidation
{
    bool success = false;
    bool endpointValid = false;
    bool moduleAddressValid = false;
    std::string endpointMessage;
    std::string moduleAddressMessage;
    std::optional<TargetModuleAddress> normalizedModuleAddress;
};

[[nodiscard]] bool operator==(
    const TunerTargetInput& left,
    const TunerTargetInput& right) noexcept;
[[nodiscard]] bool operator!=(
    const TunerTargetInput& left,
    const TunerTargetInput& right) noexcept;

[[nodiscard]] TunerTargetInputValidation ValidateTunerTargetInput(
    const TunerTargetInput& input);

// A selection contains the one normalized address used by hardware and
// workspace matching. The optional selection in TunerTargetState avoids using
// address zero as an unset sentinel.
struct TunerTargetSelection
{
    TunerTargetInput input;
    TargetModuleAddress moduleAddress;
};

enum class TargetProbeOutcome
{
    NotRun,
    InProgress,
    VerifiedIdle,
    ControllerDaqActive,
    TargetAcquisitionActive,
    WrongMvlcIdentity,
    WrongMvlcFirmware,
    WrongTargetIdentity,
    WrongTargetFirmware,
    TransportUnavailable,
    Timeout,
    MalformedResponse,
    Cancelled,
};

// Positive facts established by a read-only probe. The DAQ and target stopped
// facts stay separate here; the GUI's combined idle claim requires both.
struct TargetProbeEvidence
{
    bool controllerConnected = false;
    bool controllerIdentityAndFirmwareVerified = false;
    bool controllerDaqIdleVerified = false;
    bool targetIdentityAndFirmwareVerified = false;
    bool targetAcquisitionStoppedVerified = false;
    bool activeControllerUseDetected = false;
    bool noControlTaken = false;
    bool noVmeOrModuleSettingWritesSent = false;
};

struct TargetProbeResult
{
    TargetProbeOutcome outcome = TargetProbeOutcome::NotRun;
    TargetProbeEvidence evidence;
    bool temporaryConnectionOpened = false;
    bool temporaryConnectionClosed = false;
    std::optional<std::uint32_t> mvlcHardwareId;
    std::optional<std::uint32_t> mvlcFirmwareRevision;
    std::optional<std::uint32_t> mvlcDaqMode;
    std::optional<std::uint16_t> targetHardwareId;
    std::optional<std::uint16_t> targetFirmwareRevision;
    std::optional<std::uint16_t> targetAcquisitionControl;
    std::string message;
};

struct TunerTargetVerification
{
    bool inProgress = false;
    bool invalidated = false;
    std::optional<TunerTargetInput> probedInput;
    TargetProbeResult result;
};

enum class TunerTargetSessionGateOutcome
{
    NotRequested,
    ReadyForPreparation,
    RefusedVerificationNotFresh,
    RefusedStorageUnavailable,
};

// This is only the pre-session gate. A ready result records where future
// coordinator work will persist its evidence, but does not claim hardware
// ownership or that session preparation has started.
struct TunerTargetSessionGateResult
{
    TunerTargetSessionGateOutcome outcome =
        TunerTargetSessionGateOutcome::NotRequested;
    std::string message;
    std::string activityLogPath;
    std::string recoveryJournalPath;
};

struct TunerTargetState
{
    TunerTargetInput input;
    std::optional<TunerTargetSelection> selection;
    TunerTargetVerification verification;
    TunerTargetSessionGateResult sessionGate;
};

// Freshness compares every editable connection field, including inactive SSH
// fields. Consequently, editing the host, module address, transport kind, SSH
// destination, or remote bridge command invalidates old evidence by structure,
// even before a coordinator clears the explicit invalidated flag.
[[nodiscard]] bool TargetProbeEvidenceIsCurrent(
    const TunerTargetState& target) noexcept;
[[nodiscard]] bool TargetVerificationIsFresh(
    const TunerTargetState& target) noexcept;

// Copies only evidence owned by the target selection/probe seam. This is the
// explicit bridge from target verification into the presentation model.
void ApplyTargetPresentationEvidence(
    const TunerTargetState& target,
    TuningSessionEvidence& evidence) noexcept;

} // namespace fidget

#endif
