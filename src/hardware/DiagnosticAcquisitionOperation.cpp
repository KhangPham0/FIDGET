#include "hardware/DiagnosticAcquisitionOperation.h"

#include "hardware/VmeTransaction.h"

#include <cstdio>
#include <random>
#include <string>
#include <utility>

namespace fidget {
namespace {

constexpr std::uint16_t SupportedMdpp32HardwareId = 0x5007U;
constexpr std::uint16_t SupportedMdpp32McpdHardwareId = 0x500CU;
constexpr std::uint32_t ExpectedMvlcHardwareId = 0x5008U;
constexpr std::uint16_t ResetCommandValue = 1U;
constexpr std::uint16_t StoppedAcquisitionValue = 0U;

std::string Hexadecimal32(const std::uint32_t value)
{
    char text[16]{};
    std::snprintf(text, sizeof(text), "0x%08X", static_cast<unsigned>(value));
    return text;
}

std::string RegisterOperationError(
    const char* operation,
    const char* name,
    const std::uint16_t registerOffset,
    const std::string& error)
{
    char registerText[16]{};
    std::snprintf(
        registerText,
        sizeof(registerText),
        "%04X",
        static_cast<unsigned>(registerOffset));
    return std::string("Could not ") + operation + ' ' + name
        + " at register 0x" + registerText + ": " + error;
}

bool IsSupportedMdpp32(const std::uint16_t hardwareId)
{
    return hardwareId == SupportedMdpp32HardwareId
        || hardwareId == SupportedMdpp32McpdHardwareId;
}

std::uint32_t MakeOwnershipToken()
{
    std::random_device source;
    std::uniform_int_distribution<std::uint32_t> distribution(
        1U, 0xFFFFFFFFU);
    return distribution(source);
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
        ? "Direct acquisition startup was cancelled."
        : "Ownership could not be verified for direct acquisition startup.";
}

} // namespace

DiagnosticAcquisitionPreparationResult PrepareDiagnosticAcquisition(
    ICommandTransport& transport,
    const DiagnosticAcquisitionPreparationRequest& request,
    const std::atomic<bool>& cancellationRequested,
    const ScpCaptureOwnershipGate& ownershipGate)
{
    DiagnosticAcquisitionPreparationResult prepared;
    auto& result = prepared.acquisition;
    result.state = DiagnosticAcquisitionState::Starting;
    result.message =
        "Verifying the MDPP and preparing a direct MVLC readout stack...";
    result.baseAddress = request.targetBaseAddress;
    result.requestedChannel = request.requestedChannel;
    result.configuredModuleCount =
        request.configuredModuleBaseAddresses.size();
    result.nonTargetModuleCount = result.configuredModuleCount == 0U
        ? 0U
        : result.configuredModuleCount - 1U;

    std::string failure;
    const auto fail = [&](std::string message) {
        if (!failure.empty())
        {
            failure += ' ';
        }
        failure += std::move(message);
    };

    if (request.requestedChannel > 31U)
    {
        fail("The requested physical channel must be 0 through 31.");
    }
    if (request.host.empty() || request.commandPort == 0U
        || request.mvlcHardwareId != ExpectedMvlcHardwareId)
    {
        fail("Direct acquisition requires a verified MVLC endpoint and "
             "hardware identity.");
    }
    if (request.recoveryJournalPath.empty())
    {
        fail("Direct acquisition requires a recovery-journal path.");
    }
    if (!ownershipGate)
    {
        fail("Direct acquisition startup has no ownership gate.");
    }
    if (failure.empty())
    {
        const auto gate = ownershipGate("direct diagnostic acquisition");
        if (gate.status != ScpCaptureGateStatus::Allowed)
        {
            fail(GateFailureMessage(gate, cancellationRequested));
            result.foreignControllerDetected =
                gate.status == ScpCaptureGateStatus::OwnershipLost;
            result.cleanupSkippedToProtectForeignRun =
                result.foreignControllerDetected;
        }
    }
    if (!failure.empty())
    {
        result.state = DiagnosticAcquisitionState::Failed;
        result.message = failure;
        return prepared;
    }

    const auto readRegister = [&](const std::uint32_t baseAddress,
                                  const std::uint16_t registerOffset,
                                  std::uint16_t& destination,
                                  std::string& error) {
        const auto read = ReadVmeD16(
            transport,
            baseAddress + registerOffset,
            prepared.nextSuperReference,
            prepared.nextStackReference,
            cancellationRequested);
        if (!read.success)
        {
            error = read.error;
            return false;
        }
        destination = read.value;
        return true;
    };
    const auto writeRegister = [&](const std::uint32_t baseAddress,
                                   const std::uint16_t registerOffset,
                                   const std::uint16_t value,
                                   std::string& error) {
        const auto write = WriteVmeD16(
            transport,
            baseAddress + registerOffset,
            value,
            prepared.nextSuperReference,
            prepared.nextStackReference,
            cancellationRequested);
        if (!write.success)
        {
            error = write.error;
            return false;
        }
        return true;
    };

    std::string error;
    if (!readRegister(
            request.targetBaseAddress,
            DiagnosticHardwareIdRegister,
            result.hardwareId,
            error))
    {
        fail(RegisterOperationError(
            "read", "hardware ID", DiagnosticHardwareIdRegister, error));
    }
    else if (!IsSupportedMdpp32(result.hardwareId))
    {
        fail("The target does not identify as a supported MDPP-32 module.");
    }
    if (failure.empty()
        && !readRegister(
            request.targetBaseAddress,
            DiagnosticOutputFormatRegister,
            result.outputFormat,
            error))
    {
        fail(RegisterOperationError(
            "read", "output format", DiagnosticOutputFormatRegister, error));
    }
    if (failure.empty() && (result.outputFormat & 0x10U) == 0U)
    {
        fail("The selected MDPP is not configured for sample output. Run "
             "the read-only startup audit and the explicit verified "
             "startup-preparation transaction before direct acquisition.");
    }
    if (failure.empty()
        && !readRegister(
            request.targetBaseAddress,
            DiagnosticIrqLevelRegister,
            result.irqLevel,
            error))
    {
        fail(RegisterOperationError(
            "read", "IRQ level", DiagnosticIrqLevelRegister, error));
    }
    if (failure.empty())
    {
        auto plan = MakeMvlcSingleMdppReadoutPlan(
            request.targetBaseAddress, result.irqLevel);
        if (!plan.success)
        {
            fail(std::move(plan.error));
        }
        else
        {
            prepared.readoutPlan = std::move(plan.plan);
        }
    }

    if (failure.empty())
    {
        result.message =
            "Validating every configured non-target MDPP before installing "
            "the selected module's IRQ stack...";
        result.moduleIsolation.reserve(result.nonTargetModuleCount);

        for (const auto configuredBase :
             request.configuredModuleBaseAddresses)
        {
            if (configuredBase == request.targetBaseAddress)
            {
                continue;
            }

            DiagnosticModuleIsolation isolation;
            isolation.baseAddress = configuredBase;
            std::string moduleError;
            if (!readRegister(
                    configuredBase,
                    DiagnosticHardwareIdRegister,
                    isolation.hardwareId,
                    moduleError))
            {
                isolation.message =
                    "Could not read the configured module's hardware ID: "
                    + moduleError;
                fail("Could not validate configured MDPP at "
                     + Hexadecimal32(configuredBase) + ": " + moduleError);
            }
            else if (!IsSupportedMdpp32(isolation.hardwareId))
            {
                isolation.message =
                    "The configured address does not identify as a "
                    "supported MDPP-32 module.";
                fail("Configured address " + Hexadecimal32(configuredBase)
                     + " is not a supported MDPP-32 module.");
            }
            else if (!readRegister(
                    configuredBase,
                    DiagnosticIrqLevelRegister,
                    isolation.irqLevel,
                    moduleError))
            {
                isolation.message =
                    "Could not read the configured module's IRQ level: "
                    + moduleError;
                fail("Could not read the IRQ level at "
                     + Hexadecimal32(configuredBase) + ": " + moduleError);
            }
            else if (!readRegister(
                    configuredBase,
                    DiagnosticAcquisitionControlRegister,
                    isolation.acquisitionStateBefore,
                    moduleError))
            {
                isolation.message =
                    "Could not read the configured module's acquisition "
                    "state: " + moduleError;
                fail("Could not read the acquisition state at "
                     + Hexadecimal32(configuredBase) + ": " + moduleError);
            }
            else
            {
                isolation.validated = true;
                isolation.sharesTargetIrq =
                    isolation.irqLevel == result.irqLevel;
                isolation.stopRequired =
                    isolation.acquisitionStateBefore != 0U;
                if (isolation.stopRequired)
                {
                    ++result.activeNonTargetModulesFound;
                }
                isolation.message = isolation.stopRequired
                    ? "Validated; acquisition is active and must be "
                      "quiesced before the selected IRQ stack starts."
                    : "Validated; acquisition is already stopped, but its "
                      "FIFO and readout state will be reset.";
            }
            result.moduleIsolation.push_back(std::move(isolation));
        }
    }

    if (failure.empty())
    {
        result.message =
            "Quiescing configured non-target MDPP modules so only the "
            "selected module can drive the diagnostic IRQ stack...";
        for (auto& isolation : result.moduleIsolation)
        {
            isolation.quiescenceAttempted = true;
            std::string moduleFailure;
            std::string moduleError;
            if (isolation.stopRequired)
            {
                isolation.stopSent = writeRegister(
                    isolation.baseAddress,
                    DiagnosticAcquisitionControlRegister,
                    StoppedAcquisitionValue,
                    moduleError);
                if (!isolation.stopSent)
                {
                    moduleFailure =
                        "Could not stop acquisition: " + moduleError;
                }
            }

            if (moduleFailure.empty())
            {
                std::uint16_t stoppedReadback = 0xFFFFU;
                if (!readRegister(
                        isolation.baseAddress,
                        DiagnosticAcquisitionControlRegister,
                        stoppedReadback,
                        moduleError))
                {
                    moduleFailure =
                        "Could not verify the stopped state: " + moduleError;
                }
                else
                {
                    isolation.acquisitionStateAfter = stoppedReadback;
                    isolation.stopVerified = stoppedReadback == 0U;
                    if (!isolation.stopVerified)
                    {
                        moduleFailure =
                            "Acquisition-control readback remained "
                            + Hexadecimal32(stoppedReadback) + '.';
                    }
                }
            }

            if (isolation.stopVerified)
            {
                isolation.fifoResetSent = writeRegister(
                    isolation.baseAddress,
                    DiagnosticFifoResetRegister,
                    ResetCommandValue,
                    moduleError);
                if (!isolation.fifoResetSent)
                {
                    moduleFailure =
                        "Could not reset the stopped module's FIFO: "
                        + moduleError;
                }
                isolation.readoutResetSent = writeRegister(
                    isolation.baseAddress,
                    DiagnosticReadoutResetRegister,
                    ResetCommandValue,
                    moduleError);
                if (!isolation.readoutResetSent)
                {
                    if (!moduleFailure.empty())
                    {
                        moduleFailure += ' ';
                    }
                    moduleFailure +=
                        "Could not reset the stopped module's readout state: "
                        + moduleError;
                }
            }

            if (moduleFailure.empty() && isolation.stopVerified
                && isolation.fifoResetSent && isolation.readoutResetSent)
            {
                ++result.nonTargetModulesQuiesced;
                isolation.message =
                    "Quiesced and verified stopped; FIFO and readout state "
                    "were reset.";
            }
            else
            {
                isolation.message = moduleFailure;
                fail("Could not quiesce configured MDPP at "
                     + Hexadecimal32(isolation.baseAddress) + ": "
                     + moduleFailure);
            }
        }
    }

    if (failure.empty())
    {
        auto& record = prepared.recoveryRecord;
        record.phase = TunerRecoveryPhase::Prepared;
        record.host = request.host;
        record.commandPort = request.commandPort;
        record.mvlcHardwareId = request.mvlcHardwareId;
        record.mvlcFirmwareRevision = request.mvlcFirmwareRevision;
        record.mdppBaseAddress = request.targetBaseAddress;
        record.mdppHardwareId = result.hardwareId;
        record.mdppIrqLevel = result.irqLevel;
        record.mdppOutputFormat = result.outputFormat;
        record.stackTriggerRegister = prepared.readoutPlan.stackTriggerRegister;
        record.stackTriggerValue = prepared.readoutPlan.triggerValue;
        record.stackOffsetRegister = prepared.readoutPlan.stackOffsetRegister;
        record.stackOffsetValue = prepared.readoutPlan.stackMemoryOffset;
        record.ownershipTokenRegister = static_cast<std::uint16_t>(
            prepared.readoutPlan.stackUploadWrites.back().address + 4U);
        record.ownershipTokenValue = request.ownershipTokenValue == 0U
            ? MakeOwnershipToken()
            : request.ownershipTokenValue;
        for (const auto& isolation : result.moduleIsolation)
        {
            record.isolatedModuleBaseAddresses.push_back(
                isolation.baseAddress);
        }

        const auto saved = SaveTunerRecoveryJournal(
            record, request.recoveryJournalPath);
        if (!saved.success)
        {
            fail("Could not prepare crash recovery before installing the "
                 "diagnostic stack: " + saved.message);
        }
        else
        {
            result.recoveryJournalPrepared = true;
        }
    }

    if (!failure.empty())
    {
        result.state = DiagnosticAcquisitionState::Failed;
        result.message = failure;
        return prepared;
    }

    result.message =
        "Non-target isolation is complete and tuner ownership is journaled "
        "before the first diagnostic-stack write.";
    return prepared;
}

DiagnosticAcquisitionPreparationResult StartPreparedDiagnosticAcquisition(
    ICommandTransport& commandTransport,
    IDataReceiver& dataReceiver,
    DiagnosticAcquisitionPreparationResult prepared,
    const DiagnosticAcquisitionPreparationRequest& request,
    const std::atomic<bool>& cancellationRequested)
{
    auto& result = prepared.acquisition;
    std::string failure;
    const auto fail = [&failure](std::string message) {
        if (!failure.empty())
        {
            failure += ' ';
        }
        failure += std::move(message);
    };

    if (result.state != DiagnosticAcquisitionState::Starting
        || !result.recoveryJournalPrepared)
    {
        fail("Diagnostic acquisition cannot start without completed "
             "isolation and a prepared recovery journal.");
    }
    if (request.commandPort == 0xFFFFU)
    {
        fail("The MVLC command port has no adjacent data port.");
    }
    if (!failure.empty())
    {
        result.state = DiagnosticAcquisitionState::Failed;
        result.message = failure;
        return prepared;
    }

    result.dataPort = static_cast<std::uint16_t>(request.commandPort + 1U);
    const auto opened = dataReceiver.Open(request.host, result.dataPort);
    if (!opened.success)
    {
        fail("Could not open the MVLC data path: " + opened.error);
    }

    if (failure.empty())
    {
        const std::array<std::uint32_t, 2> redirectWords{
            MvlcCommandBufferStart,
            MvlcCommandBufferEnd,
        };
        const auto redirect = EncodeMvlcWordsLittleEndian(
            redirectWords.data(), redirectWords.size());
        const auto sent = dataReceiver.Send(
            redirect.data(), redirect.size());
        if (!sent.success)
        {
            fail("Could not redirect the MVLC Ethernet data stream: "
                 + sent.error);
        }
    }

    const auto writeLocal = [&](const MvlcLocalRegisterWrite* writes,
                                const std::size_t writeCount,
                                std::string message) {
        const auto written = WriteLocalRegisters(
            commandTransport,
            writes,
            writeCount,
            prepared.nextSuperReference,
            cancellationRequested);
        if (!written.success)
        {
            fail(std::move(message) + written.error);
            return false;
        }
        return true;
    };
    const auto writeVme = [&](const std::uint16_t registerOffset,
                              const std::uint16_t value,
                              const char* name) {
        const auto written = WriteVmeD16(
            commandTransport,
            result.baseAddress + registerOffset,
            value,
            prepared.nextSuperReference,
            prepared.nextStackReference,
            cancellationRequested);
        if (!written.success)
        {
            fail(RegisterOperationError(
                "write", name, registerOffset, written.error));
            return false;
        }
        return true;
    };

    if (failure.empty())
    {
        auto stackInstallWrites = prepared.readoutPlan.stackUploadWrites;
        stackInstallWrites.push_back({
            prepared.recoveryRecord.ownershipTokenRegister,
            prepared.recoveryRecord.ownershipTokenValue,
        });
        static_cast<void>(writeLocal(
            stackInstallWrites.data(),
            stackInstallWrites.size(),
            "Could not upload the diagnostic readout stack and unique "
            "ownership token: "));
    }
    if (failure.empty())
    {
        const std::array<MvlcLocalRegisterWrite, 2> setupWrites{{
            {
                prepared.readoutPlan.stackOffsetRegister,
                prepared.readoutPlan.stackMemoryOffset,
            },
            {prepared.readoutPlan.stackTriggerRegister, 0U},
        }};
        static_cast<void>(writeLocal(
            setupWrites.data(),
            setupWrites.size(),
            "Could not prepare the diagnostic readout stack: "));
    }
    if (failure.empty())
    {
        const MvlcLocalRegisterWrite triggerWrite{
            prepared.readoutPlan.stackTriggerRegister,
            prepared.readoutPlan.triggerValue,
        };
        static_cast<void>(writeLocal(
            &triggerWrite,
            1U,
            "Could not enable the diagnostic IRQ stack: "));
    }

    struct MdppControlWrite
    {
        std::uint16_t registerOffset;
        std::uint16_t value;
        const char* name;
    };
    constexpr std::array<MdppControlWrite, 4> StartSequence{{
        {DiagnosticAcquisitionControlRegister, 0U, "stop acquisition"},
        {DiagnosticFifoResetRegister, 1U, "reset FIFO"},
        {DiagnosticReadoutResetRegister, 1U, "reset readout"},
        {DiagnosticAcquisitionControlRegister, 1U, "start acquisition"},
    }};
    for (const auto& write : StartSequence)
    {
        if (!failure.empty() || cancellationRequested.load())
        {
            break;
        }
        static_cast<void>(writeVme(
            write.registerOffset, write.value, write.name));
    }

    if (failure.empty() && !cancellationRequested.load())
    {
        const MvlcLocalRegisterWrite daqWrite{
            DiagnosticDaqModeRegister,
            DiagnosticDaqEnableValue,
        };
        static_cast<void>(writeLocal(
            &daqWrite,
            1U,
            "Could not enable MVLC DAQ mode: "));
    }

    if (failure.empty() && !cancellationRequested.load())
    {
        constexpr std::array<std::uint16_t, 1> DaqModeAddress{
            DiagnosticDaqModeRegister,
        };
        const auto daqMode = ReadLocalRegisters(
            commandTransport,
            DaqModeAddress.data(),
            DaqModeAddress.size(),
            prepared.nextSuperReference,
            cancellationRequested);
        if (!daqMode.success)
        {
            fail("Could not verify MVLC DAQ mode: " + daqMode.error);
        }
        else if (daqMode.values.empty()
                 || (daqMode.values.front() & 0x1U) == 0U)
        {
            fail("MVLC DAQ mode did not enable autonomous-stack bit 0.");
        }
    }

    if (failure.empty() && !cancellationRequested.load())
    {
        prepared.recoveryRecord.phase = TunerRecoveryPhase::Active;
        const auto saved = SaveTunerRecoveryJournal(
            prepared.recoveryRecord, request.recoveryJournalPath);
        if (!saved.success)
        {
            fail("DAQ mode was enabled, but the active recovery record "
                 "could not be committed: " + saved.message);
        }
        else
        {
            result.recoveryJournalActive = true;
        }
    }
    if (cancellationRequested.load() && failure.empty())
    {
        fail("Direct acquisition startup was cancelled.");
    }

    if (!failure.empty())
    {
        result.state = DiagnosticAcquisitionState::Failed;
        result.message = failure;
        dataReceiver.Close();
        return prepared;
    }

    result.state = DiagnosticAcquisitionState::Running;
    result.message =
        "Direct diagnostic acquisition is running with every configured "
        "non-target MDPP quiesced. The selected module's sample source and "
        "all filtering registers are unchanged.";
    return prepared;
}

} // namespace fidget
