#include "hardware/DiagnosticRecoveryOperation.h"

#include "core/RecoveryJournal.h"
#include "core/ScpConfiguration.h"
#include "core/ScpRegistry.h"
#include "hardware/DiagnosticAcquisitionOperation.h"
#include "hardware/VmeTransaction.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>

namespace fidget {
namespace {

constexpr std::uint16_t SampleConfigurationRegister = 0x614AU;
constexpr std::uint16_t FirmwareRevisionRegister = 0x600EU;
constexpr auto SelectorSettleTime = std::chrono::microseconds(50);
constexpr auto FrontendSettleTime = std::chrono::microseconds(20);

std::string Hexadecimal32(const std::uint32_t value)
{
    char buffer[16]{};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "0x%08X",
        static_cast<unsigned>(value));
    return buffer;
}

void AddStep(
    DiagnosticOrphanRecoveryResult& result,
    std::string name,
    const bool success,
    std::string message)
{
    result.steps.push_back({
        std::move(name), success, std::move(message)});
}

} // namespace

DiagnosticOrphanRecoveryResult RecoverDiagnosticOrphan(
    ICommandTransport& transport,
    const DiagnosticOrphanRecoveryRequest& request,
    const std::atomic<bool>& cancellationRequested)
{
    DiagnosticOrphanRecoveryResult result;
    result.state = DiagnosticOrphanRecoveryState::Recovering;
    result.message = "Verifying the journaled tuner fingerprint...";
    auto record = request.record;
    auto nextSuperReference = request.nextSuperReference;
    auto nextStackReference = request.nextStackReference;

    const std::array<std::uint16_t, 2> identityAddresses{{
        TunerRecoveryMvlcHardwareIdRegister,
        TunerRecoveryMvlcFirmwareRegister,
    }};
    const auto identity = ReadLocalRegisters(
        transport,
        identityAddresses.data(),
        identityAddresses.size(),
        nextSuperReference,
        cancellationRequested);
    if (!identity.success || identity.values.size() != 2U)
    {
        result.state = DiagnosticOrphanRecoveryState::Failed;
        result.message = identity.success
            ? "The MVLC identity response had the wrong value count."
            : "Could not read the MVLC identity for recovery: "
                + identity.error;
        AddStep(result, "fingerprint", false, result.message);
        return result;
    }

    const auto expected = BuildTunerRecoveryFingerprintExpectation(record);
    if (!expected.success)
    {
        result.state = DiagnosticOrphanRecoveryState::Failed;
        result.message = expected.message;
        AddStep(result, "fingerprint", false, result.message);
        return result;
    }
    const auto fingerprintRead = ReadLocalRegisters(
        transport,
        expected.addresses.data(),
        expected.addresses.size(),
        nextSuperReference,
        cancellationRequested);
    if (!fingerprintRead.success
        || fingerprintRead.values.size() != expected.values.size())
    {
        result.state = DiagnosticOrphanRecoveryState::Failed;
        result.message = fingerprintRead.success
            ? "The tuner fingerprint response had the wrong value count."
            : "Could not read the complete tuner fingerprint for recovery: "
                + fingerprintRead.error;
        AddStep(result, "fingerprint", false, result.message);
        return result;
    }

    TunerRecoveryLiveFingerprint live;
    live.mvlcHardwareId = identity.values[0];
    live.mvlcFirmwareRevision = identity.values[1];
    for (std::size_t index = 0U; index < live.values.size(); ++index)
    {
        live.values[index] = fingerprintRead.values[index];
    }
    result.fingerprint = EvaluateTunerRecoveryFingerprint(record, live);
    if (result.fingerprint.verdict
        == TunerRecoveryFingerprintVerdict::ForeignOrMismatched)
    {
        result.state = DiagnosticOrphanRecoveryState::ForeignOrMismatched;
        result.message = result.fingerprint.message;
        AddStep(result, "fingerprint", false, result.message);
        return result;
    }
    AddStep(result, "fingerprint", true, result.fingerprint.message);

    if (result.fingerprint.verdict
        == TunerRecoveryFingerprintVerdict::AlreadyClean)
    {
        std::string removeError;
        if (!RemoveTunerRecoveryJournal(
                request.recoveryJournalPath, removeError))
        {
            result.state = DiagnosticOrphanRecoveryState::Failed;
            result.message =
                "The crate is already clean, but the stale recovery journal could not be removed: "
                + removeError;
            AddStep(result, "journal", false, result.message);
            return result;
        }
        result.journalRemoved = true;
        result.state = DiagnosticOrphanRecoveryState::AlreadyClean;
        result.message = result.fingerprint.message;
        AddStep(
            result,
            "journal",
            true,
            "Removed the stale recovery journal without a hardware write.");
        return result;
    }
    const bool idleRestoration = result.fingerprint.verdict
        == TunerRecoveryFingerprintVerdict::IdleWithRestoration;
    std::string error;
    const auto fail = [&](std::string message) {
        result.state = DiagnosticOrphanRecoveryState::Failed;
        result.message = std::move(message);
        AddStep(result, "recovery", false, result.message);
        return result;
    };
    const auto writeVme = [&](const std::uint32_t baseAddress,
                              const std::uint16_t registerOffset,
                              const std::uint16_t value) {
        const auto written = WriteVmeD16(
            transport,
            baseAddress + registerOffset,
            value,
            nextSuperReference,
            nextStackReference,
            cancellationRequested);
        if (written.success)
        {
            result.hardwareWriteSent = true;
        }
        return written;
    };
    const auto readVme = [&](const std::uint32_t baseAddress,
                             const std::uint16_t registerOffset) {
        return ReadVmeD16(
            transport,
            baseAddress + registerOffset,
            nextSuperReference,
            nextStackReference,
            cancellationRequested);
    };
    const auto writeAndVerify = [&](const std::uint32_t baseAddress,
                                    const std::uint16_t registerOffset,
                                    const std::uint16_t value,
                                    const char* name,
                                    std::string& error) {
        const auto written = writeVme(baseAddress, registerOffset, value);
        if (!written.success)
        {
            error = std::string("Could not write ") + name + ": "
                + written.error;
            return false;
        }
        const auto readback = readVme(baseAddress, registerOffset);
        if (!readback.success)
        {
            error = std::string("Could not verify ") + name + ": "
                + readback.error;
            return false;
        }
        if (readback.value != value)
        {
            error = std::string(name) + " readback was "
                + std::to_string(readback.value) + ", expected "
                + std::to_string(value) + ".";
            return false;
        }
        return true;
    };
    const auto requireIdleDaqMode = [&](const char* boundary) {
        const std::array<std::uint16_t, 1> addresses{{
            TunerRecoveryDaqModeRegister,
        }};
        const auto daqMode = ReadLocalRegisters(
            transport,
            addresses.data(),
            addresses.size(),
            nextSuperReference,
            cancellationRequested);
        if (!daqMode.success || daqMode.values.size() != 1U)
        {
            error = daqMode.success
                ? std::string(
                      "The MVLC DAQ-mode response had the wrong value count ")
                    + boundary + ". No further recovery write was sent."
                : std::string("Could not reverify MVLC DAQ mode ")
                    + boundary + ": " + daqMode.error
                    + ". No further recovery write was sent.";
            return false;
        }
        if (daqMode.values[0] != 0U)
        {
            error = "MVLC DAQ mode changed to "
                + Hexadecimal32(daqMode.values[0]) + " " + boundary
                + ". Recovery stopped without another hardware write.";
            return false;
        }
        return true;
    };

    if (idleRestoration
        && !requireIdleDaqMode("before idle restoration"))
    {
        return fail(std::move(error));
    }
    if (idleRestoration)
    {
        AddStep(
            result,
            "daq_mode",
            true,
            "Reverified that MVLC DAQ mode remains zero.");
    }

    const auto targetHardwareId = readVme(
        record.mdppBaseAddress, DiagnosticHardwareIdRegister);
    if (!targetHardwareId.success)
    {
        return fail(
            "Could not verify the journaled MDPP identity before recovery: "
            + targetHardwareId.error);
    }
    const auto targetFirmware = readVme(
        record.mdppBaseAddress, FirmwareRevisionRegister);
    if (!targetFirmware.success)
    {
        return fail(
            "Could not verify the MDPP firmware before recovery: "
            + targetFirmware.error);
    }
    if (targetHardwareId.value != record.mdppHardwareId)
    {
        return fail(
            "Journaled MDPP hardware ID mismatch: expected "
            + Hexadecimal32(record.mdppHardwareId) + ", read "
            + Hexadecimal32(targetHardwareId.value)
            + ". No recovery write was sent.");
    }
    if (!IsWriteApprovedMdpp32HardwareId(targetHardwareId.value))
    {
        return fail(
            targetHardwareId.value == Mdpp32AlternateHardwareId
                ? "MDPP-32 v2 recovery writes await recorded hardware "
                  "acceptance. No recovery write was sent; retain the journal."
                : "The journaled target is not a write-approved MDPP-32. "
                  "No recovery write was sent; retain the journal.");
    }
    if (targetFirmware.value != Mdpp32ScpFirmwareRevisionFw2051)
    {
        return fail(
            "Unsupported MDPP firmware during recovery: expected exact FW2051 "
            + Hexadecimal32(Mdpp32ScpFirmwareRevisionFw2051)
            + ", read " + Hexadecimal32(targetFirmware.value)
            + ". No recovery write was sent.");
    }
    AddStep(
        result,
        "target_identity",
        true,
        "Verified the journaled MDPP hardware identity and exact FW2051 "
        "firmware.");

    if (record.sourceRestoreRequired
        && !record.sourceAppliedConfigurationAvailable)
    {
        return fail(
            "The recovery journal predates format version 4 and does not "
            "record the applied waveform-source value. FIDGET cannot prove "
            "a safe restoration value. No recovery write was sent; retain "
            "the journal and resolve the source setting manually.");
    }
    const auto* previewDefinition = record.previewRestoreRequired
        ? FindFw2051ScpSetting(record.previewRegisterOffset)
        : nullptr;
    if (record.previewRestoreRequired && previewDefinition == nullptr)
    {
        return fail(
            "The journaled preview register "
            + Hexadecimal32(record.previewRegisterOffset)
            + " is not in the FW2051 SCP recovery allowlist. No VME-bus "
              "recovery write was sent; retain the journal.");
    }

    if (idleRestoration)
    {
        const auto targetState = readVme(
            record.mdppBaseAddress,
            DiagnosticAcquisitionControlRegister);
        if (!targetState.success)
        {
            return fail(
                "Could not verify that the journaled MDPP remains stopped "
                "before idle restoration: "
                + targetState.error);
        }
        if (targetState.value != 0U)
        {
            return fail(
                "The journaled MDPP acquisition state is "
                + Hexadecimal32(targetState.value)
                + ", expected zero. No recovery write was sent; "
                + "retain the journal.");
        }
        AddStep(
            result,
            "target_state",
            true,
            "Verified that the journaled MDPP remains stopped.");
    }

    if (!idleRestoration)
    {
        for (const auto baseAddress : record.isolatedModuleBaseAddresses)
        {
            const auto isolatedHardwareId = readVme(
                baseAddress, DiagnosticHardwareIdRegister);
            if (!isolatedHardwareId.success)
            {
                return fail(
                    "Could not verify the isolated MDPP identity at "
                    + Hexadecimal32(baseAddress)
                    + " before recovery: " + isolatedHardwareId.error);
            }
            if (!IsWriteApprovedMdpp32HardwareId(
                    isolatedHardwareId.value))
            {
                return fail(
                    "Unsupported isolated-module hardware ID at "
                    + Hexadecimal32(baseAddress) + ": read "
                    + Hexadecimal32(isolatedHardwareId.value)
                    + ". No recovery write was sent.");
            }
            AddStep(
                result,
                "isolated_identity",
                true,
                "Verified the isolated MDPP identity at "
                    + Hexadecimal32(baseAddress) + ".");
        }
    }

    if (!idleRestoration
        && !writeAndVerify(
                record.mdppBaseAddress,
                DiagnosticAcquisitionControlRegister,
                0U,
                "selected-module stop",
                error))
    {
        return fail(std::move(error));
    }
    if (!idleRestoration)
    {
        result.targetStopped = true;
        AddStep(
            result,
            "target",
            true,
            "Stopped and verified the selected MDPP.");
    }

    if (!idleRestoration)
    {
        for (const auto baseAddress : record.isolatedModuleBaseAddresses)
        {
            if (!writeAndVerify(
                    baseAddress,
                    DiagnosticAcquisitionControlRegister,
                    0U,
                    "isolated-module stop",
                    error))
            {
                return fail(
                    "Could not recover isolated module "
                    + Hexadecimal32(baseAddress) + ": " + error);
            }
            const auto fifo = writeVme(
                baseAddress, DiagnosticFifoResetRegister, 1U);
            if (!fifo.success)
            {
                return fail(
                    "Could not reset the isolated-module FIFO at "
                    + Hexadecimal32(baseAddress) + ": " + fifo.error);
            }
            const auto readout = writeVme(
                baseAddress, DiagnosticReadoutResetRegister, 1U);
            if (!readout.success)
            {
                return fail(
                    "Could not reset isolated-module readout at "
                    + Hexadecimal32(baseAddress) + ": " + readout.error);
            }
            ++result.isolatedModulesRecovered;
            AddStep(
                result,
                "isolated",
                true,
                "Stopped, verified, and reset isolated module "
                    + Hexadecimal32(baseAddress) + ".");
        }
    }

    const auto restoreBanked = [&](const bool preview,
                                   const std::uint16_t quad,
                                   const std::uint16_t registerOffset,
                                   const std::uint16_t originalValue,
                                   const std::uint16_t expectedLiveValue,
                                   const bool requireExpectedLive,
                                   const bool requireIdleGate) {
        error.clear();
        if (requireIdleGate
            && !requireIdleDaqMode("before selecting a recovery quad"))
        {
            return false;
        }
        const auto selected = writeVme(
            record.mdppBaseAddress, Fw2051ScpSelectorRegister, quad);
        if (!selected.success)
        {
            error = "Could not select recovery quad: " + selected.error;
            return false;
        }
        std::this_thread::sleep_for(SelectorSettleTime);
        const auto liveValue = readVme(record.mdppBaseAddress, registerOffset);
        const std::string valueName = preview
            ? "parameter preview"
            : "waveform source";
        std::string dependencyError;
        if (liveValue.success
            && preview
            && (!requireExpectedLive
                || liveValue.value == expectedLiveValue
                || liveValue.value == originalValue)
            && liveValue.value != originalValue
            && previewDefinition != nullptr
            && previewDefinition->dependencyRule
                != Fw2051ScpDependencyRule::None)
        {
            const auto dependency = readVme(
                record.mdppBaseAddress,
                previewDefinition->dependencyRegister);
            if (!dependency.success)
            {
                dependencyError =
                    "Could not read the live recovery dependency "
                    + std::string(previewDefinition->dependencyName)
                    + ": " + dependency.error;
            }
            else if (!Fw2051ScpDependencySatisfied(
                         *previewDefinition,
                         originalValue,
                         dependency.value))
            {
                dependencyError = "The original "
                    + std::string(previewDefinition->name) + " value "
                    + std::to_string(originalValue) + " must be "
                    + Fw2051ScpDependencyRelation(
                        previewDefinition->dependencyRule)
                    + " the live " + previewDefinition->dependencyName
                    + " value " + std::to_string(dependency.value)
                    + "; recovery refused the now-invalid restore.";
            }
        }
        if (requireIdleGate
            && !requireIdleDaqMode("before restoring or parking the selector"))
        {
            return false;
        }
        if (!liveValue.success)
        {
            error = "Could not read the live recovery value: "
                + liveValue.error;
        }
        else if (requireExpectedLive
                 && liveValue.value != expectedLiveValue
                 && liveValue.value != originalValue)
        {
            error = "The live " + valueName + " value is "
                + std::to_string(liveValue.value) + ", expected "
                + std::to_string(expectedLiveValue)
                + "; recovery refused to overwrite the unexpected value.";
        }
        else if (!dependencyError.empty())
        {
            error = std::move(dependencyError);
        }
        else if (liveValue.value != originalValue)
        {
            if (preview)
            {
                result.previewRestoreAttempted = true;
            }
            else
            {
                result.sourceRestoreAttempted = true;
            }
            const auto restored = writeVme(
                record.mdppBaseAddress, registerOffset, originalValue);
            if (!restored.success)
            {
                error = "Could not restore the journaled value: "
                    + restored.error;
            }
            else
            {
                std::this_thread::sleep_for(FrontendSettleTime);
                const auto readback = readVme(
                    record.mdppBaseAddress, registerOffset);
                if (!readback.success)
                {
                    error = "Could not verify the restored value: "
                        + readback.error;
                }
                else if (readback.value != originalValue)
                {
                    error = "Restored value readback was "
                        + std::to_string(readback.value) + ", expected "
                        + std::to_string(originalValue) + ".";
                }
                else if (preview)
                {
                    result.previewRestoreVerified = true;
                }
                else
                {
                    result.sourceRestoreVerified = true;
                }
            }
        }
        else if (preview)
        {
            result.previewRestoreVerified = true;
        }
        else
        {
            result.sourceRestoreVerified = true;
        }

        if (requireIdleGate
            && !requireIdleDaqMode("before parking the recovery selector"))
        {
            return false;
        }
        const auto parked = writeVme(
            record.mdppBaseAddress, Fw2051ScpSelectorRegister, 0U);
        if (!parked.success)
        {
            const std::string parkingError =
                "Could not park the recovery selector: " + parked.error;
            error = error.empty()
                ? parkingError
                : error + " " + parkingError;
        }
        if (parked.success)
        {
            std::this_thread::sleep_for(SelectorSettleTime);
        }
        return error.empty();
    };

    if (record.previewRestoreRequired)
    {
        if (!restoreBanked(
                true,
                record.previewQuad,
                record.previewRegisterOffset,
                record.previewOriginalValue,
                record.previewAppliedValue,
                true,
                idleRestoration))
        {
            return fail(std::move(error));
        }
        record.previewRestoreRequired = false;
        record.previewQuad = 0U;
        record.previewRegisterOffset = 0U;
        record.previewOriginalValue = 0U;
        record.previewAppliedValue = 0U;
        const auto saved = SaveTunerRecoveryJournal(
            record, request.recoveryJournalPath);
        if (!saved.success)
        {
            return fail(
                "The preview was restored, but its journal state could not be cleared: "
                + saved.message);
        }
        AddStep(result, "preview", true, "Restored the parameter preview.");
    }

    if (record.sourceRestoreRequired)
    {
        if (!restoreBanked(
                false,
                record.sourceQuad,
                SampleConfigurationRegister,
                record.sourceOriginalConfiguration,
                record.sourceAppliedConfiguration,
                true,
                idleRestoration))
        {
            return fail(std::move(error));
        }
        record.sourceRestoreRequired = false;
        record.sourceQuad = 0U;
        record.sourceOriginalConfiguration = 0U;
        record.sourceAppliedConfigurationAvailable = false;
        record.sourceAppliedConfiguration = 0U;
        const auto saved = SaveTunerRecoveryJournal(
            record, request.recoveryJournalPath);
        if (!saved.success)
        {
            return fail(
                "The waveform source was restored, but its journal state "
                "could not be cleared: "
                + saved.message);
        }
        AddStep(result, "source", true, "Restored the waveform source.");
    }

    if (idleRestoration)
    {
        std::string removeError;
        if (!RemoveTunerRecoveryJournal(
                request.recoveryJournalPath, removeError))
        {
            return fail(
                "All journaled values were restored, but the recovery "
                "journal could not be removed: "
                + removeError);
        }
        result.journalRemoved = true;
        AddStep(result, "journal", true, "Removed the recovery journal.");
        result.state = DiagnosticOrphanRecoveryState::Recovered;
        result.message =
            "Restored and verified the journaled tuner state while MVLC DAQ "
            "mode remained zero.";
        return result;
    }

    const std::array<MvlcLocalRegisterWrite, 4> cleanupWrites{{
        {TunerRecoveryDaqModeRegister, 0U},
        {record.stackTriggerRegister, 0U},
        {record.stackOffsetRegister, 0U},
        {record.ownershipTokenRegister, 0U},
    }};
    const auto cleaned = WriteLocalRegisters(
        transport,
        cleanupWrites.data(),
        cleanupWrites.size(),
        nextSuperReference,
        cancellationRequested);
    if (!cleaned.success)
    {
        return fail("Could not clear the MVLC recovery fingerprint: "
                    + cleaned.error);
    }
    result.hardwareWriteSent = true;
    for (const auto& cleanup : cleanupWrites)
    {
        const auto readback = ReadLocalRegisters(
            transport,
            &cleanup.address,
            1U,
            nextSuperReference,
            cancellationRequested);
        if (!readback.success || readback.values.size() != 1U
            || readback.values[0] != 0U)
        {
            return fail(readback.success
                ? "An MVLC recovery-cleanup readback was not zero."
                : "Could not verify MVLC recovery cleanup: "
                    + readback.error);
        }
    }
    result.mvlcCleanupVerified = true;
    AddStep(result, "mvlc", true, "Cleared and verified the MVLC fingerprint.");

    const auto fifo = writeVme(
        record.mdppBaseAddress, DiagnosticFifoResetRegister, 1U);
    if (!fifo.success)
    {
        return fail("Could not reset the selected-module FIFO: "
                    + fifo.error);
    }
    const auto readout = writeVme(
        record.mdppBaseAddress, DiagnosticReadoutResetRegister, 1U);
    if (!readout.success)
    {
        return fail("Could not reset selected-module readout: "
                    + readout.error);
    }
    result.targetReset = true;
    AddStep(result, "target_reset", true, "Reset selected-module FIFO and readout.");

    std::string removeError;
    if (!RemoveTunerRecoveryJournal(
            request.recoveryJournalPath, removeError))
    {
        return fail(
            "Hardware recovery passed, but the journal could not be removed: "
            + removeError);
    }
    result.journalRemoved = true;
    AddStep(result, "journal", true, "Removed the recovery journal.");
    result.state = DiagnosticOrphanRecoveryState::Recovered;
    result.message =
        "Recovered the tuner-owned diagnostic orphan and verified every cleanup readback.";
    return result;
}

} // namespace fidget
