#include "hardware/ScpSingleRepairOperation.h"

#include "core/ScpConfiguration.h"
#include "core/ScpRegistry.h"
#include "hardware/VmeTransaction.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>

namespace fidget {
namespace {

constexpr std::uint16_t HardwareIdRegister = 0x6008U;
constexpr std::uint16_t FirmwareRevisionRegister = 0x600EU;
constexpr auto SelectorSettleTime = std::chrono::microseconds(50);
constexpr auto FrontendSettleTime = std::chrono::microseconds(20);
constexpr std::uint16_t InitialSuperReference = 0x3A00U;
constexpr std::uint32_t InitialStackReference = 0x9D100001U;

std::string RegisterOperationError(
    const char* operation,
    const std::string& name,
    const std::uint16_t registerOffset,
    const std::string& error)
{
    char registerText[16]{};
    std::snprintf(
        registerText,
        sizeof(registerText),
        "%04X",
        static_cast<unsigned>(registerOffset));
    return std::string("Could not ") + operation + ' ' + name +
        " at register 0x" + registerText + ": " + error;
}

std::string GateFailureMessage(
    const ScpCaptureGateResult& gate,
    const std::atomic<bool>& cancellationRequested)
{
    if (!gate.message.empty())
    {
        return gate.message;
    }
    return cancellationRequested.load()
        ? "The SCP profile repair was cancelled."
        : "Ownership could not be verified for the SCP profile repair.";
}

} // namespace

ScpSingleRepairResult RepairFw2051ScpProfileValue(
    ICommandTransport& transport,
    const ScpSingleRepairRequest& request,
    const std::atomic<bool>& cancellationRequested,
    const ScpCaptureOwnershipGate& ownershipGate,
    const ScpSingleRepairDelay& settleDelay)
{
    ScpSingleRepairResult result;
    result.state = ScpSingleRepairState::Applying;
    result.message =
        "Verifying and stopping the module if needed before applying one "
        "profile value...";
    result.baseAddress = request.baseAddress;
    result.selectedQuad = request.quad;
    result.registerOffset = request.registerOffset;
    result.expectedLiveValue = request.expectedLiveValue;
    result.profileValue = request.profileValue;

    const auto* definition = FindFw2051ScpSetting(request.registerOffset);
    result.settingName = definition != nullptr
        ? definition->name
        : "Unsupported SCP parameter";

    std::string failure;
    const auto appendFailure = [&failure](std::string message) {
        if (!failure.empty())
        {
            failure += ' ';
        }
        failure += std::move(message);
    };
    const auto waitForSettle = [&](const std::chrono::microseconds duration) {
        if (settleDelay)
        {
            settleDelay(duration);
        }
        else
        {
            std::this_thread::sleep_for(duration);
        }
    };

    if ((request.baseAddress & 0xFFFFU) != 0U)
    {
        appendFailure(
            "The MDPP base must be a full 64-KiB-aligned A32 address.");
    }
    else if (request.quad >= Fw2051ScpQuadCount)
    {
        appendFailure("The SCP channel quad must be between 0 and 7.");
    }
    else if (definition == nullptr)
    {
        appendFailure(
            "Retained profile repair is not available for this register.");
    }
    else
    {
        appendFailure(ValidateFw2051ScpProfileValue(
            request.registerOffset, request.profileValue));
    }

    if (!failure.empty())
    {
        result.state = ScpSingleRepairState::Failed;
        result.message = failure;
        return result;
    }
    if (!ownershipGate)
    {
        result.state = ScpSingleRepairState::Failed;
        result.message = "The SCP profile repair has no ownership gate.";
        return result;
    }

    std::uint16_t nextSuperReference = InitialSuperReference;
    std::uint32_t nextStackReference = InitialStackReference;
    bool ownershipAvailable = true;
    const auto reverifyOwnership = [&](const std::string& operationName) {
        if (!ownershipAvailable)
        {
            return false;
        }
        const auto gate = ownershipGate(operationName);
        if (gate.status == ScpCaptureGateStatus::Allowed)
        {
            return true;
        }
        result.communicationUnavailable = gate.status ==
            ScpCaptureGateStatus::CommunicationUnavailable;
        ownershipAvailable = false;
        appendFailure(GateFailureMessage(gate, cancellationRequested));
        return false;
    };

    const auto readRegister = [&](
                                  const std::uint16_t registerOffset,
                                  std::uint16_t& destination,
                                  std::string& error) {
        const auto read = ReadVmeD16(
            transport,
            request.baseAddress + registerOffset,
            nextSuperReference,
            nextStackReference,
            cancellationRequested);
        if (!read.success)
        {
            error = read.error;
            return false;
        }
        destination = read.value;
        return true;
    };
    const auto writeRegister = [&](
                                   const std::uint16_t registerOffset,
                                   const std::uint16_t value,
                                   std::string& error) {
        const auto write = WriteVmeD16(
            transport,
            request.baseAddress + registerOffset,
            value,
            nextSuperReference,
            nextStackReference,
            cancellationRequested);
        if (!write.success)
        {
            error = write.error;
            return false;
        }
        return true;
    };

    if (!reverifyOwnership("the retained SCP profile repair"))
    {
        result.state = ScpSingleRepairState::Failed;
        result.message = failure;
        return result;
    }

    std::string error;
    std::uint16_t hardwareId = 0U;
    std::uint16_t firmwareRevision = 0U;
    if (!readRegister(HardwareIdRegister, hardwareId, error))
    {
        appendFailure(RegisterOperationError(
            "read", "hardware ID", HardwareIdRegister, error));
    }
    else if (hardwareId != Mdpp32HardwareId &&
             hardwareId != Mdpp32AlternateHardwareId)
    {
        appendFailure(
            "The target does not identify as a supported MDPP-32 module.");
    }

    if (failure.empty() &&
        !readRegister(FirmwareRevisionRegister, firmwareRevision, error))
    {
        appendFailure(RegisterOperationError(
            "read", "firmware revision", FirmwareRevisionRegister, error));
    }
    else if (failure.empty() &&
             firmwareRevision != Mdpp32ScpFirmwareRevisionFw2051)
    {
        appendFailure(
            "The target does not report supported SCP FW2051 firmware; "
            "the SCP profile value was not applied.");
    }

    std::uint16_t acquisitionState = 0U;
    if (failure.empty() &&
        !readRegister(
            Fw2051AcquisitionControlRegister,
            acquisitionState,
            error))
    {
        appendFailure(RegisterOperationError(
            "read",
            "acquisition enable",
            Fw2051AcquisitionControlRegister,
            error));
    }
    else if (failure.empty() &&
             acquisitionState == Fw2051StopAcquisitionValue)
    {
        result.moduleStopVerified = true;
    }
    else if (failure.empty() &&
             reverifyOwnership("the SCP profile module stop"))
    {
        if (!writeRegister(
                Fw2051AcquisitionControlRegister,
                Fw2051StopAcquisitionValue,
                error))
        {
            appendFailure(RegisterOperationError(
                "write",
                "stop acquisition",
                Fw2051AcquisitionControlRegister,
                error));
        }
        else
        {
            result.moduleStopSent = true;
            std::uint16_t stopReadback = 0xFFFFU;
            if (!readRegister(
                    Fw2051AcquisitionControlRegister,
                    stopReadback,
                    error))
            {
                appendFailure(RegisterOperationError(
                    "verify",
                    "stopped acquisition",
                    Fw2051AcquisitionControlRegister,
                    error));
            }
            else if (stopReadback != Fw2051StopAcquisitionValue)
            {
                appendFailure(
                    "Acquisition-enable readback was " +
                    std::to_string(stopReadback) +
                    ", expected zero after Stop. No further write was "
                    "sent.");
            }
            else
            {
                result.moduleStopVerified = true;
            }
        }
    }

    bool selectorWasWritten = false;
    if (failure.empty() && result.moduleStopVerified &&
        reverifyOwnership("the SCP profile bank selection"))
    {
        if (!writeRegister(
                Fw2051ScpSelectorRegister, request.quad, error))
        {
            appendFailure(RegisterOperationError(
                "write",
                "channel-quad selector",
                Fw2051ScpSelectorRegister,
                error));
        }
        else
        {
            selectorWasWritten = true;
            waitForSettle(SelectorSettleTime);
        }
    }

    if (failure.empty() &&
        definition->dependencyRule != Fw2051ScpDependencyRule::None)
    {
        result.dependencyName = definition->dependencyName;
        if (!readRegister(
                definition->dependencyRegister,
                result.dependencyValue,
                error))
        {
            appendFailure(RegisterOperationError(
                "read",
                result.dependencyName + " dependency",
                definition->dependencyRegister,
                error));
        }
        else
        {
            result.dependencyChecked = true;
            if (!Fw2051ScpDependencySatisfied(
                    *definition,
                    request.profileValue,
                    result.dependencyValue))
            {
                appendFailure(
                    "The profile " + result.settingName + " must be " +
                    Fw2051ScpDependencyRelation(
                        definition->dependencyRule) +
                    " the live " + result.dependencyName + " (profile " +
                    std::to_string(request.profileValue) + ", live " +
                    std::to_string(result.dependencyValue) +
                    "); no profile write was sent.");
            }
        }
    }

    if (failure.empty())
    {
        if (!readRegister(
                request.registerOffset, result.capturedLiveValue, error))
        {
            appendFailure(RegisterOperationError(
                "read", result.settingName, request.registerOffset, error));
        }
        else
        {
            result.liveValueCaptured = true;
        }
    }

    if (failure.empty() &&
        result.capturedLiveValue != request.expectedLiveValue)
    {
        appendFailure(
            "The live " + result.settingName +
            " changed after the snapshot (captured " +
            std::to_string(result.capturedLiveValue) + ", expected " +
            std::to_string(request.expectedLiveValue) +
            "). No profile write was sent; capture all eight quads again.");
    }

    if (failure.empty() && reverifyOwnership("the SCP profile write"))
    {
        result.writeAttempted = true;
        if (!writeRegister(
                request.registerOffset, request.profileValue, error))
        {
            appendFailure(RegisterOperationError(
                "write", result.settingName, request.registerOffset, error));
        }
        else
        {
            waitForSettle(FrontendSettleTime);
            if (!readRegister(
                    request.registerOffset, result.appliedReadback, error))
            {
                appendFailure(RegisterOperationError(
                    "verify",
                    result.settingName,
                    request.registerOffset,
                    error));
            }
            else if (result.appliedReadback != request.profileValue)
            {
                appendFailure(
                    "The applied " + result.settingName +
                    " readback was " +
                    std::to_string(result.appliedReadback) + ", expected " +
                    std::to_string(request.profileValue) + '.');
            }
            else
            {
                result.writeVerified = true;
                result.profileValueRetained = true;
            }
        }
    }

    if (result.writeAttempted && !result.writeVerified &&
        result.liveValueCaptured &&
        reverifyOwnership("the SCP profile rollback"))
    {
        result.rollbackAttempted = true;
        if (!writeRegister(
                request.registerOffset, result.capturedLiveValue, error))
        {
            appendFailure(RegisterOperationError(
                "rollback",
                result.settingName,
                request.registerOffset,
                error));
        }
        else
        {
            waitForSettle(FrontendSettleTime);
            if (!readRegister(
                    request.registerOffset,
                    result.rollbackReadback,
                    error))
            {
                appendFailure(RegisterOperationError(
                    "verify rollback",
                    result.settingName,
                    request.registerOffset,
                    error));
            }
            else if (result.rollbackReadback
                     != result.capturedLiveValue)
            {
                appendFailure(
                    "The rollback readback was " +
                    std::to_string(result.rollbackReadback) +
                    ", expected " +
                    std::to_string(result.capturedLiveValue) + '.');
            }
            else
            {
                result.rollbackVerified = true;
                result.profileValueRetained = false;
            }
        }
    }

    if (selectorWasWritten &&
        reverifyOwnership("the SCP profile selector parking"))
    {
        if (!writeRegister(Fw2051ScpSelectorRegister, 0U, error))
        {
            appendFailure(RegisterOperationError(
                "park",
                "channel-quad selector",
                Fw2051ScpSelectorRegister,
                error));
        }
        else
        {
            result.selectorParkedAtQuadZero = true;
            waitForSettle(SelectorSettleTime);
        }
    }

    if (result.writeAttempted && result.moduleStopVerified &&
        ownershipAvailable &&
        reverifyOwnership("the SCP profile FIFO reset"))
    {
        if (!writeRegister(
                Fw2051FifoResetRegister,
                Fw2051ResetCommandValue,
                error))
        {
            appendFailure(RegisterOperationError(
                "write", "reset FIFO", Fw2051FifoResetRegister, error));
        }
        else
        {
            result.fifoResetSent = true;
        }
    }

    if (result.writeAttempted && result.moduleStopVerified &&
        ownershipAvailable &&
        reverifyOwnership("the SCP profile readout reset"))
    {
        if (!writeRegister(
                Fw2051ReadoutResetRegister,
                Fw2051ResetCommandValue,
                error))
        {
            appendFailure(RegisterOperationError(
                "write",
                "reset readout",
                Fw2051ReadoutResetRegister,
                error));
        }
        else
        {
            result.readoutResetSent = true;
        }
    }

    if (result.moduleStopVerified && ownershipAvailable &&
        reverifyOwnership("the SCP profile final stopped-state verification"))
    {
        std::uint16_t stoppedReadback = 0xFFFFU;
        if (!readRegister(
                Fw2051AcquisitionControlRegister,
                stoppedReadback,
                error))
        {
            appendFailure(RegisterOperationError(
                "verify",
                "final stopped acquisition",
                Fw2051AcquisitionControlRegister,
                error));
        }
        else if (stoppedReadback != Fw2051StopAcquisitionValue)
        {
            appendFailure(
                "The module did not remain stopped after the SCP profile "
                "repair.");
        }
        else
        {
            result.moduleLeftStopped = true;
        }
    }

    const bool passed = failure.empty() && result.writeVerified &&
        result.profileValueRetained && result.moduleStopVerified &&
        result.selectorParkedAtQuadZero && result.fifoResetSent &&
        result.readoutResetSent && result.moduleLeftStopped;
    result.state = passed
        ? ScpSingleRepairState::Passed
        : ScpSingleRepairState::Failed;
    if (passed)
    {
        result.message =
            "Applied and retained profile " + result.settingName +
            " with exact readback. The selector was parked at quad 0, the "
            "FIFO and readout resets were sent, and the module remains "
            "stopped; recapture all eight quads before comparing or applying "
            "another value.";
    }
    else if (result.rollbackVerified && result.moduleLeftStopped)
    {
        result.message = failure +
            " The captured live value was restored with exact readback.";
        if (result.selectorParkedAtQuadZero && result.fifoResetSent &&
            result.readoutResetSent)
        {
            result.message +=
                " The selector was parked at quad 0 and the FIFO and readout "
                "resets were sent.";
        }
        result.message += " The module remains stopped.";
    }
    else if (!failure.empty() && result.moduleLeftStopped)
    {
        result.message = failure + " The module remains stopped.";
    }
    else if (failure.empty())
    {
        result.message =
            "The SCP profile repair did not complete all safety checks.";
    }
    else
    {
        result.message = failure;
    }
    return result;
}

} // namespace fidget
