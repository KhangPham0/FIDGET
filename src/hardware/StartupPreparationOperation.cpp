#include "hardware/StartupPreparationOperation.h"

#include "core/ScpConfiguration.h"
#include "hardware/VmeTransaction.h"

#include <cstdio>
#include <string>
#include <utility>
#include <vector>

namespace fidget {
namespace {

constexpr std::uint16_t InitialSuperReference = 0x1700U;
constexpr std::uint32_t InitialStackReference = 0x9C0C0001U;
constexpr std::uint16_t HardwareIdRegister = 0x6008U;
constexpr std::uint16_t FirmwareRevisionRegister = 0x600EU;

std::string Hexadecimal32(const std::uint32_t value)
{
    char text[16]{};
    std::snprintf(text, sizeof(text), "0x%08X", static_cast<unsigned>(value));
    return text;
}

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

std::string ValidateAudit(
    const std::uint32_t baseAddress,
    const StartupAuditResult& audit)
{
    if (audit.state != StartupAuditState::Complete)
    {
        return "Module startup preparation requires a completed startup "
               "audit for the selected module.";
    }
    if (audit.baseAddress != baseAddress)
    {
        return "The completed startup audit does not match the selected "
               "VME base address.";
    }
    if (audit.hardwareId != Mdpp32HardwareId ||
        audit.firmwareRevision != Mdpp32ScpFirmwareRevisionFw2051)
    {
        return "No VME write was sent: startup preparation is enabled only "
               "for the tested MDPP-32 hardware 0x5007 with SCP FW2051.";
    }
    return {};
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
        ? "Module startup preparation was cancelled."
        : "Ownership could not be verified for module startup preparation.";
}

} // namespace

StartupPreparationResult PrepareFw2051ModuleForStartup(
    ICommandTransport& transport,
    const std::uint32_t baseAddress,
    const StartupAuditResult& audit,
    const std::atomic<bool>& cancellationRequested,
    const ScpCaptureOwnershipGate& ownershipGate)
{
    StartupPreparationResult result;
    result.state = StartupPreparationState::Preparing;
    result.message =
        "Validating exact hardware/firmware and capturing the tuner "
        "readout contract before the first write...";
    result.baseAddress = baseAddress;
    result.registers.reserve(Fw2051StartupPreparationRegisterCount);

    std::string failure = ValidateAudit(baseAddress, audit);
    if (!failure.empty())
    {
        result.state = StartupPreparationState::Failed;
        result.message = failure;
        return result;
    }
    if (!ownershipGate)
    {
        result.state = StartupPreparationState::Failed;
        result.message = "Module startup preparation has no ownership gate.";
        return result;
    }

    std::uint16_t nextSuperReference = InitialSuperReference;
    std::uint32_t nextStackReference = InitialStackReference;
    bool ownershipAvailable = true;
    const auto appendFailure = [&failure](std::string message) {
        if (!failure.empty())
        {
            failure += ' ';
        }
        failure += std::move(message);
    };
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
        ownershipAvailable = false;
        appendFailure(GateFailureMessage(gate, cancellationRequested));
        return false;
    };
    const auto readRegister = [&](const std::uint16_t registerOffset,
                                  std::uint16_t& destination,
                                  std::string& error) {
        const auto read = ReadVmeD16(
            transport,
            baseAddress + registerOffset,
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
    const auto writeRegister = [&](const std::uint16_t registerOffset,
                                   const std::uint16_t value,
                                   std::string& error) {
        const auto write = WriteVmeD16(
            transport,
            baseAddress + registerOffset,
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

    if (!reverifyOwnership("module startup preparation preflight"))
    {
        result.state = StartupPreparationState::Failed;
        result.message = failure;
        return result;
    }

    std::string error;
    if (!readRegister(HardwareIdRegister, result.hardwareId, error))
    {
        appendFailure(RegisterOperationError(
            "read", "hardware ID", HardwareIdRegister, error));
    }
    if (failure.empty() &&
        !readRegister(
            FirmwareRevisionRegister,
            result.firmwareRevision,
            error))
    {
        appendFailure(RegisterOperationError(
            "read", "firmware revision", FirmwareRevisionRegister, error));
    }
    if (failure.empty() &&
        (result.hardwareId != Mdpp32HardwareId ||
         result.firmwareRevision != Mdpp32ScpFirmwareRevisionFw2051))
    {
        appendFailure(
            "No VME write was sent: startup preparation is enabled only "
            "for the tested MDPP-32 hardware 0x5007 with SCP FW2051. "
            "The selected module reported hardware " +
            Hexadecimal32(result.hardwareId) + " and firmware " +
            Hexadecimal32(result.firmwareRevision) + '.');
    }
    result.strictFirmwareAccepted = failure.empty();

    for (const auto& definition : Fw2051StartupPreparationRegisterTable)
    {
        if (!failure.empty())
        {
            break;
        }
        std::uint16_t currentValue = 0U;
        if (!readRegister(definition.registerOffset, currentValue, error))
        {
            appendFailure(RegisterOperationError(
                "read", definition.name, definition.registerOffset, error));
            break;
        }
        result.registers.push_back({
            definition.registerOffset,
            definition.name,
            currentValue,
            definition.targetValue,
            currentValue,
        });
        auto& setting = result.registers.back();
        setting.changeRequired = currentValue != definition.targetValue;
        ++result.settingsRead;
        if (setting.changeRequired)
        {
            ++result.changedSettings;
        }
    }

    if (failure.empty() &&
        !readRegister(
            Fw2051AcquisitionControlRegister,
            result.originalAcquisitionValue,
            error))
    {
        appendFailure(RegisterOperationError(
            "read",
            "acquisition enable",
            Fw2051AcquisitionControlRegister,
            error));
    }
    if (!failure.empty())
    {
        result.state = StartupPreparationState::Failed;
        result.message = failure;
        return result;
    }

    if (reverifyOwnership("module startup preparation stop"))
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
                    ", expected zero after Stop.");
            }
            else
            {
                result.moduleStopVerified = true;
                result.moduleLeftStopped = true;
            }
        }
    }

    std::vector<std::size_t> attemptedSettings;
    for (std::size_t index = 0U;
         failure.empty() && ownershipAvailable &&
         index < result.registers.size();
         ++index)
    {
        auto& setting = result.registers[index];
        if (!setting.changeRequired)
        {
            continue;
        }
        if (!reverifyOwnership(
                "module startup preparation " + setting.name + " write"))
        {
            break;
        }

        setting.writeAttempted = true;
        attemptedSettings.push_back(index);
        ++result.writesAttempted;
        if (!writeRegister(
                setting.registerOffset,
                setting.targetValue,
                error))
        {
            appendFailure(RegisterOperationError(
                "write", setting.name, setting.registerOffset, error));
            break;
        }
        if (!readRegister(
                setting.registerOffset,
                setting.appliedReadback,
                error))
        {
            appendFailure(RegisterOperationError(
                "verify", setting.name, setting.registerOffset, error));
            break;
        }
        if (setting.appliedReadback != setting.targetValue)
        {
            appendFailure(
                setting.name + " readback was " +
                std::to_string(setting.appliedReadback) + ", expected " +
                std::to_string(setting.targetValue) + '.');
            break;
        }
        setting.writeVerified = true;
        ++result.writesVerified;
    }

    if (failure.empty() && reverifyOwnership(
            "module startup preparation FIFO/readout reset"))
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
    }

    if (failure.empty() && reverifyOwnership(
            "module startup preparation final verification"))
    {
        std::uint16_t acquisitionReadback = 0xFFFFU;
        if (!readRegister(
                Fw2051AcquisitionControlRegister,
                acquisitionReadback,
                error))
        {
            appendFailure(RegisterOperationError(
                "verify",
                "stopped acquisition",
                Fw2051AcquisitionControlRegister,
                error));
        }
        else if (acquisitionReadback != Fw2051StopAcquisitionValue)
        {
            result.moduleLeftStopped = false;
            appendFailure(
                "Acquisition-enable changed during final verification.");
        }

        for (const auto& setting : result.registers)
        {
            if (!failure.empty())
            {
                break;
            }
            std::uint16_t finalReadback = 0U;
            if (!readRegister(
                    setting.registerOffset,
                    finalReadback,
                    error))
            {
                appendFailure(RegisterOperationError(
                    "final verify",
                    setting.name,
                    setting.registerOffset,
                    error));
                break;
            }
            if (finalReadback != setting.targetValue)
            {
                appendFailure(
                    "Final " + setting.name + " readback was " +
                    std::to_string(finalReadback) + ", expected " +
                    std::to_string(setting.targetValue) + '.');
                break;
            }
        }
    }

    if (!failure.empty() && !attemptedSettings.empty())
    {
        result.state = StartupPreparationState::RollingBack;
        result.rollbackAttempted = true;
        bool rollbackSafe = ownershipAvailable;
        for (auto attempted = attemptedSettings.rbegin();
             attempted != attemptedSettings.rend();
             ++attempted)
        {
            auto& setting = result.registers[*attempted];
            if (!ownershipAvailable || !reverifyOwnership(
                    "module startup preparation " + setting.name +
                    " rollback"))
            {
                rollbackSafe = false;
                break;
            }
            setting.rollbackAttempted = true;
            ++result.rollbackWritesAttempted;
            if (!writeRegister(
                    setting.registerOffset,
                    setting.originalValue,
                    error))
            {
                appendFailure(RegisterOperationError(
                    "rollback", setting.name, setting.registerOffset, error));
                rollbackSafe = false;
                continue;
            }
            if (!readRegister(
                    setting.registerOffset,
                    setting.rollbackReadback,
                    error))
            {
                appendFailure(RegisterOperationError(
                    "verify rollback",
                    setting.name,
                    setting.registerOffset,
                    error));
                rollbackSafe = false;
                continue;
            }
            if (setting.rollbackReadback != setting.originalValue)
            {
                appendFailure(
                    "Rollback " + setting.name + " readback was " +
                    std::to_string(setting.rollbackReadback) +
                    ", expected " + std::to_string(setting.originalValue) +
                    '.');
                rollbackSafe = false;
                continue;
            }
            setting.rollbackVerified = true;
            ++result.rollbackWritesVerified;
        }
        result.rollbackVerified = rollbackSafe &&
            result.rollbackWritesVerified == attemptedSettings.size();
    }

    if (ownershipAvailable && result.moduleStopSent &&
        reverifyOwnership(
            "module startup preparation stopped-state verification"))
    {
        std::uint16_t stoppedReadback = 0xFFFFU;
        if (readRegister(
                Fw2051AcquisitionControlRegister,
                stoppedReadback,
                error))
        {
            result.moduleLeftStopped =
                stoppedReadback == Fw2051StopAcquisitionValue;
            if (!result.moduleLeftStopped)
            {
                appendFailure(
                    "The module did not remain stopped after startup "
                    "preparation.");
            }
        }
        else
        {
            result.moduleLeftStopped = false;
            appendFailure(RegisterOperationError(
                "verify",
                "final stopped acquisition",
                Fw2051AcquisitionControlRegister,
                error));
        }
    }

    const bool passed = failure.empty() && result.strictFirmwareAccepted &&
        result.moduleStopVerified &&
        result.writesVerified == result.changedSettings &&
        result.fifoResetSent && result.readoutResetSent &&
        result.moduleLeftStopped;
    result.state = passed
        ? StartupPreparationState::Passed
        : StartupPreparationState::Failed;
    result.message = passed
        ? "Prepared the selected FW2051 SCP module for deterministic "
          "sampled standard-streaming readout. Verified " +
            std::to_string(result.changedSettings) +
            " changed setting(s); experiment I/O, timestamps, event "
            "marking/TDC choices, and all banked detector settings were not "
            "addressed. The module remains stopped."
        : result.rollbackVerified
            ? failure +
                " Every attempted module-wide setting was restored with "
                "exact readback; acquisition remains stopped."
            : failure.empty()
                ? "Module startup preparation did not reach a fully "
                  "verified safe state."
                : failure;
    return result;
}

} // namespace fidget
