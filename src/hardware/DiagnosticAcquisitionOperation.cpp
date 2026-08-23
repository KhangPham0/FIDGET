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
    const ScpCaptureOwnershipGate& ownershipGate,
    const DiagnosticAcquisitionJournalSaver& journalSaver)
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
            result.communicationUncertain =
                gate.status == ScpCaptureGateStatus::CommunicationUnavailable;
            if (result.communicationUncertain)
            {
                ++result.commandPathFailures;
            }
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

        const auto saved = journalSaver
            ? journalSaver(record, request.recoveryJournalPath)
            : SaveTunerRecoveryJournal(
                record, request.recoveryJournalPath);
        if (!saved.success)
        {
            fail("Could not prepare crash recovery before changing any "
                 "configured module state: " + saved.message);
        }
        else
        {
            result.recoveryJournalPrepared = true;
        }
    }

    if (failure.empty())
    {
        const auto gate = ownershipGate("non-target MDPP quiescence");
        if (gate.status != ScpCaptureGateStatus::Allowed)
        {
            fail(GateFailureMessage(gate, cancellationRequested));
            result.foreignControllerDetected =
                gate.status == ScpCaptureGateStatus::OwnershipLost;
            result.cleanupSkippedToProtectForeignRun =
                result.foreignControllerDetected;
            result.communicationUncertain =
                gate.status == ScpCaptureGateStatus::CommunicationUnavailable;
            if (result.communicationUncertain)
            {
                ++result.commandPathFailures;
            }
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

    if (!failure.empty())
    {
        if (result.recoveryJournalPrepared)
        {
            result.orphanRecoveryRequired = true;
            failure += " The Prepared recovery journal was retained.";
        }
        result.state = DiagnosticAcquisitionState::Failed;
        result.message = failure;
        return prepared;
    }

    result.message =
        "Tuner ownership was journaled before non-target isolation, and "
        "isolation is complete before the first diagnostic-stack write.";
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

DiagnosticFingerprintResult VerifyDiagnosticOwnershipFingerprint(
    ICommandTransport& commandTransport,
    const DiagnosticAcquisitionPreparationResult& prepared,
    std::uint16_t& nextReference,
    const std::atomic<bool>& cancellationRequested)
{
    DiagnosticFingerprintResult result;
    std::vector<std::uint16_t> addresses{
        DiagnosticDaqModeRegister,
        prepared.readoutPlan.stackTriggerRegister,
        prepared.readoutPlan.stackOffsetRegister,
    };
    addresses.reserve(4U + prepared.readoutPlan.stackUploadWrites.size());
    for (const auto& stackWord : prepared.readoutPlan.stackUploadWrites)
    {
        addresses.push_back(stackWord.address);
    }
    addresses.push_back(prepared.recoveryRecord.ownershipTokenRegister);

    const auto read = ReadLocalRegisters(
        commandTransport,
        addresses.data(),
        addresses.size(),
        nextReference,
        cancellationRequested);
    if (!read.success)
    {
        result.message =
            "Could not read the complete tuner fingerprint in one MVLC "
            "transaction: " + read.error;
        return result;
    }

    result.daqMode = read.values[0];
    if ((result.daqMode & 0x1U) == 0U)
    {
        result.outcome = DiagnosticFingerprintOutcome::ForeignFingerprint;
        result.message =
            "MVLC DAQ mode no longer has autonomous-stack bit 0 enabled "
            "(readback " + Hexadecimal32(result.daqMode) + ").";
        return result;
    }

    const auto verifyLocal = [&](const std::size_t index,
                                 const std::uint16_t address,
                                 const std::uint32_t expected,
                                 const char* name) {
        const std::uint32_t actual = read.values[index];
        if (actual == expected)
        {
            return true;
        }
        result.outcome = DiagnosticFingerprintOutcome::ForeignFingerprint;
        result.message = std::string(name) + " changed at "
            + Hexadecimal32(address) + ": expected "
            + Hexadecimal32(expected) + ", read "
            + Hexadecimal32(actual) + ".";
        return false;
    };

    if (!verifyLocal(
            1U,
            prepared.readoutPlan.stackTriggerRegister,
            prepared.readoutPlan.triggerValue,
            "Diagnostic stack trigger")
        || !verifyLocal(
            2U,
            prepared.readoutPlan.stackOffsetRegister,
            prepared.readoutPlan.stackMemoryOffset,
            "Diagnostic stack offset"))
    {
        return result;
    }

    std::size_t valueIndex = 3U;
    for (const auto& stackWord : prepared.readoutPlan.stackUploadWrites)
    {
        if (!verifyLocal(
                valueIndex++,
                stackWord.address,
                stackWord.value,
                "Diagnostic stack word"))
        {
            return result;
        }
    }
    if (!verifyLocal(
            valueIndex,
            prepared.recoveryRecord.ownershipTokenRegister,
            prepared.recoveryRecord.ownershipTokenValue,
            "Unique tuner ownership token"))
    {
        return result;
    }

    result.outcome = DiagnosticFingerprintOutcome::Verified;
    result.message = "The complete unique tuner fingerprint matches.";
    return result;
}

DiagnosticAcquisitionPreparationResult StopDiagnosticAcquisition(
    ICommandTransport& commandTransport,
    IDataReceiver& dataReceiver,
    DiagnosticAcquisitionPreparationResult prepared,
    const DiagnosticAcquisitionPreparationRequest& request,
    const std::atomic<bool>& cancellationRequested,
    const DiagnosticStopOwnershipCheck ownershipCheck)
{
    auto& result = prepared.acquisition;
    result.state = DiagnosticAcquisitionState::Stopping;
    std::string failure;
    const auto fail = [&failure](std::string message) {
        if (!failure.empty())
        {
            failure += ' ';
        }
        failure += std::move(message);
    };
    const auto writeVme = [&](const std::uint32_t baseAddress,
                              const std::uint16_t registerOffset,
                              const std::uint16_t value,
                              const char* name) {
        const auto written = WriteVmeD16(
            commandTransport,
            baseAddress + registerOffset,
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
    const auto readVme = [&](const std::uint32_t baseAddress,
                             const std::uint16_t registerOffset,
                             std::uint16_t& value,
                             const char* name) {
        const auto read = ReadVmeD16(
            commandTransport,
            baseAddress + registerOffset,
            prepared.nextSuperReference,
            prepared.nextStackReference,
            cancellationRequested);
        if (!read.success)
        {
            fail(RegisterOperationError(
                "read", name, registerOffset, read.error));
            return false;
        }
        value = read.value;
        return true;
    };

    if (ownershipCheck == DiagnosticStopOwnershipCheck::Required)
    {
        const auto fingerprint = VerifyDiagnosticOwnershipFingerprint(
            commandTransport,
            prepared,
            prepared.nextSuperReference,
            cancellationRequested);
        ++result.ownershipHeartbeatChecks;
        if (fingerprint.outcome
            == DiagnosticFingerprintOutcome::ForeignFingerprint)
        {
            result.foreignControllerDetected = true;
            result.cleanupSkippedToProtectForeignRun = true;
            result.state = DiagnosticAcquisitionState::Failed;
            result.message =
                "Tuner ownership was lost: " + fingerprint.message
                + " The tuner detached passively. No MDPP stop, DAQ-mode "
                  "write, or readout-stack cleanup was sent, so a possible "
                  "MVME run was left untouched.";
            dataReceiver.Close();
            commandTransport.Close();
            return prepared;
        }
        if (fingerprint.outcome
            == DiagnosticFingerprintOutcome::CommunicationUnavailable)
        {
            result.communicationUncertain = true;
            ++result.commandPathFailures;
            result.orphanRecoveryRequired = true;
            result.state = DiagnosticAcquisitionState::Failed;
            result.message =
                "The command path remained unavailable, so ownership could not "
                "be proven before cleanup. No blind hardware write was sent. "
                "The unique tuner fingerprint is journaled for recovery. Last "
                "error: " + fingerprint.message;
            dataReceiver.Close();
            commandTransport.Close();
            return prepared;
        }
    }

    if (writeVme(
            result.baseAddress,
            DiagnosticAcquisitionControlRegister,
            0U,
            "stop acquisition"))
    {
        result.moduleStopSent = true;
        std::uint16_t stoppedReadback = 0xFFFFU;
        if (readVme(
                result.baseAddress,
                DiagnosticAcquisitionControlRegister,
                stoppedReadback,
                "stopped acquisition")
            && stoppedReadback != 0U)
        {
            fail("Selected-module acquisition-control readback was not "
                 "zero after Stop.");
        }
    }

    result.nonTargetModulesVerifiedStoppedOnCleanup = 0U;
    for (auto& isolation : result.moduleIsolation)
    {
        if (!isolation.quiescenceAttempted)
        {
            continue;
        }
        std::uint16_t acquisitionReadback = 0xFFFFU;
        const std::size_t failureLengthBefore = failure.size();
        if (!readVme(
                isolation.baseAddress,
                DiagnosticAcquisitionControlRegister,
                acquisitionReadback,
                "non-target acquisition state during cleanup"))
        {
            isolation.message = failure.substr(failureLengthBefore);
            continue;
        }

        const bool restarted = acquisitionReadback != 0U;
        if (restarted)
        {
            isolation.stopRequired = true;
            isolation.stopSent = writeVme(
                isolation.baseAddress,
                DiagnosticAcquisitionControlRegister,
                0U,
                "non-target stop during cleanup");
            if (isolation.stopSent)
            {
                static_cast<void>(readVme(
                    isolation.baseAddress,
                    DiagnosticAcquisitionControlRegister,
                    acquisitionReadback,
                    "non-target stopped state during cleanup"));
            }
        }
        isolation.acquisitionStateAfter = acquisitionReadback;
        isolation.stopVerified = acquisitionReadback == 0U;
        if (isolation.stopVerified && restarted)
        {
            isolation.fifoResetSent = writeVme(
                isolation.baseAddress,
                DiagnosticFifoResetRegister,
                1U,
                "non-target FIFO reset during cleanup");
            isolation.readoutResetSent = writeVme(
                isolation.baseAddress,
                DiagnosticReadoutResetRegister,
                1U,
                "non-target readout reset during cleanup");
        }
        isolation.cleanupVerified = isolation.stopVerified
            && isolation.fifoResetSent && isolation.readoutResetSent;
        if (isolation.cleanupVerified)
        {
            ++result.nonTargetModulesVerifiedStoppedOnCleanup;
            isolation.message = restarted
                ? "Unexpectedly restarted, then stopped, reset, and "
                  "verified during cleanup."
                : "Verified stopped during cleanup.";
        }
        else
        {
            fail("Non-target cleanup failed at "
                 + Hexadecimal32(isolation.baseAddress) + ".");
        }
    }

    const std::array<MvlcLocalRegisterWrite, 4> cleanupWrites{{
        {DiagnosticDaqModeRegister, 0U},
        {prepared.readoutPlan.stackTriggerRegister, 0U},
        {prepared.readoutPlan.stackOffsetRegister, 0U},
        {prepared.recoveryRecord.ownershipTokenRegister, 0U},
    }};
    const auto cleaned = WriteLocalRegisters(
        commandTransport,
        cleanupWrites.data(),
        cleanupWrites.size(),
        prepared.nextSuperReference,
        cancellationRequested);
    if (!cleaned.success)
    {
        fail("Could not disable MVLC DAQ mode and the diagnostic stack: "
             + cleaned.error);
    }
    else
    {
        bool allZero = true;
        for (const auto& cleanup : cleanupWrites)
        {
            const auto readback = ReadLocalRegisters(
                commandTransport,
                &cleanup.address,
                1U,
                prepared.nextSuperReference,
                cancellationRequested);
            if (!readback.success || readback.values.empty()
                || readback.values.front() != 0U)
            {
                allZero = false;
                fail(readback.success
                    ? "MVLC cleanup readback was not zero."
                    : "Could not verify MVLC cleanup: " + readback.error);
                break;
            }
        }
        result.daqModeDisabled = allZero;
        result.readoutStackDisabled = allZero;
    }

    dataReceiver.Close();
    if (prepared.recoveryRecord.previewRestoreRequired)
    {
        result.orphanRecoveryRequired = true;
        fail(
            "The temporary parameter preview could not be restored. The "
            "selected module and MVLC stack were cleaned up, but the "
            "recovery journal was retained for the next-launch recovery flow.");
    }
    if (prepared.recoveryRecord.sourceRestoreRequired)
    {
        result.orphanRecoveryRequired = true;
        fail(
            "The temporary waveform source could not be restored. The "
            "selected module and MVLC stack were cleaned up, but the "
            "recovery journal was retained for the next-launch recovery flow.");
    }
    const bool stoppedCleanly = failure.empty()
        && result.moduleStopSent
        && result.daqModeDisabled
        && result.readoutStackDisabled
        && result.nonTargetModulesVerifiedStoppedOnCleanup
            == result.nonTargetModulesQuiesced;
    if (stoppedCleanly)
    {
        std::string journalError;
        if (!RemoveTunerRecoveryJournal(
                request.recoveryJournalPath, journalError))
        {
            fail("Hardware stopped cleanly, but the recovery journal could "
                 "not be removed: " + journalError);
        }
        else
        {
            result.recoveryJournalRemoved = true;
            result.recoveryJournalPrepared = false;
            result.recoveryJournalActive = false;
        }
    }

    result.state = failure.empty() && stoppedCleanly
        ? DiagnosticAcquisitionState::Stopped
        : DiagnosticAcquisitionState::Failed;
    result.message = result.state == DiagnosticAcquisitionState::Stopped
        ? "Direct acquisition stopped cleanly. The selected MDPP and all "
          "isolated non-target modules are stopped, MVLC DAQ mode is zero, "
          "and the diagnostic stack is disabled."
        : failure.empty()
            ? "Direct acquisition cleanup did not pass every readback check."
            : failure;
    return prepared;
}

} // namespace fidget
