#include "hardware/DiagnosticPreviewOperation.h"

#include "core/RecoveryJournal.h"
#include "core/ScpConfiguration.h"
#include "core/ScpRegistry.h"
#include "hardware/DiagnosticPauseResume.h"
#include "hardware/VmeTransaction.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>

namespace fidget {
namespace {

constexpr auto SelectorSettleTime = std::chrono::microseconds(50);
constexpr auto FrontendSettleTime = std::chrono::microseconds(20);

enum class PreviewAction
{
    Apply,
    Restore,
};

struct PreviewTransactionRequest
{
    PreviewAction action = PreviewAction::Apply;
    std::uint16_t selectedQuad = 0U;
    std::uint16_t registerOffset = 0U;
    std::uint16_t requestedValue = 0U;
    std::uint16_t originalValue = 0U;
    bool resumeAfterTransaction = true;
    bool automaticallyRestoredOnStop = false;
};

void AppendFailure(std::string& destination, std::string message)
{
    if (!destination.empty())
    {
        destination += ' ';
    }
    destination += std::move(message);
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
    return std::string("Could not ") + operation + ' ' + name
        + " at register 0x" + registerText + ": " + error;
}

bool SavePreviewRecoveryState(
    DiagnosticAcquisitionPreparationResult& session,
    const std::string& path,
    const bool active,
    const PreviewTransactionRequest& request,
    const std::uint16_t originalValue,
    std::string& error)
{
    auto record = session.recoveryRecord;
    record.previewRestoreRequired = active;
    record.previewQuad = active ? request.selectedQuad : 0U;
    record.previewRegisterOffset = active ? request.registerOffset : 0U;
    record.previewOriginalValue = active ? originalValue : 0U;
    record.previewAppliedValue = active ? request.requestedValue : 0U;
    const auto saved = SaveTunerRecoveryJournal(record, path);
    if (!saved.success)
    {
        error = saved.message;
        return false;
    }
    session.recoveryRecord = std::move(record);
    return true;
}

DiagnosticParameterPreviewResult ExecutePreviewTransaction(
    ICommandTransport& transport,
    DiagnosticAcquisitionPreparationResult& session,
    const PreviewTransactionRequest& request,
    const std::string& recoveryJournalPath,
    const std::atomic<bool>& cancellationRequested)
{
    const auto startedAt = std::chrono::steady_clock::now();
    const bool applying = request.action == PreviewAction::Apply;
    DiagnosticParameterPreviewResult result;
    result.state = applying
        ? DiagnosticParameterPreviewState::Applying
        : DiagnosticParameterPreviewState::Restoring;
    result.selectedQuad = request.selectedQuad;
    result.registerOffset = request.registerOffset;
    result.requestedValue = request.requestedValue;
    result.originalValue = request.originalValue;
    result.automaticallyRestoredOnStop =
        request.automaticallyRestoredOnStop;

    const auto* definition = FindFw2051ScpSetting(request.registerOffset);
    result.settingName = definition == nullptr
        ? "Unsupported SCP parameter"
        : definition->name;
    result.message = applying
        ? "Pausing acquisition and applying the " + result.settingName
            + " preview..."
        : request.automaticallyRestoredOnStop
            ? "Stop requested: restoring the original "
                + result.settingName + " before cleanup..."
            : "Pausing acquisition and restoring the original "
                + result.settingName + "...";

    if (definition == nullptr)
    {
        result.state = DiagnosticParameterPreviewState::Failed;
        result.message =
            "Temporary preview is not available for this SCP register.";
        return result;
    }
    if (request.selectedQuad >= Fw2051ScpQuadCount)
    {
        result.state = DiagnosticParameterPreviewState::Failed;
        result.message = "The SCP channel quad must be between 0 and 7.";
        return result;
    }
    if (session.acquisition.state != DiagnosticAcquisitionState::Running)
    {
        result.state = DiagnosticParameterPreviewState::Failed;
        result.message =
            "A parameter can be previewed only while direct acquisition is running.";
        return result;
    }
    if (recoveryJournalPath.empty())
    {
        result.state = DiagnosticParameterPreviewState::Failed;
        result.message =
            "A recovery-journal path is required before a parameter preview.";
        return result;
    }
    if (applying)
    {
        const auto validation = ValidateFw2051ScpSettingValue(
            *definition, request.requestedValue, "preview ");
        if (!validation.empty())
        {
            result.state = DiagnosticParameterPreviewState::Failed;
            result.message = validation;
            return result;
        }
    }

    auto fingerprintReference = session.nextSuperReference;
    const auto fingerprint = VerifyDiagnosticOwnershipFingerprint(
        transport,
        session,
        fingerprintReference,
        cancellationRequested);
    session.nextSuperReference = fingerprintReference;
    if (fingerprint.outcome != DiagnosticFingerprintOutcome::Verified)
    {
        result.foreignFingerprint = fingerprint.outcome
            == DiagnosticFingerprintOutcome::ForeignFingerprint;
        result.communicationUnavailable = fingerprint.outcome
            == DiagnosticFingerprintOutcome::CommunicationUnavailable;
        result.state = DiagnosticParameterPreviewState::Failed;
        result.message = fingerprint.message;
        return result;
    }
    result.fingerprintVerified = true;

    std::string failure;
    const auto paused = PauseDiagnosticDataTaking(
        transport,
        session.acquisition.baseAddress,
        session.nextSuperReference,
        session.nextStackReference,
        cancellationRequested);
    result.modulePaused = paused.modulePaused;
    result.daqModePaused = paused.daqModePaused;
    AppendFailure(failure, paused.error);

    const auto writeVme = [&](const std::uint16_t registerOffset,
                              const std::uint16_t value) {
        return WriteVmeD16(
            transport,
            session.acquisition.baseAddress + registerOffset,
            value,
            session.nextSuperReference,
            session.nextStackReference,
            cancellationRequested);
    };
    const auto readVme = [&](const std::uint16_t registerOffset) {
        return ReadVmeD16(
            transport,
            session.acquisition.baseAddress + registerOffset,
            session.nextSuperReference,
            session.nextStackReference,
            cancellationRequested);
    };

    bool selectorSelected = false;
    if (result.modulePaused && result.daqModePaused)
    {
        const auto selected = writeVme(
            Fw2051ScpSelectorRegister, request.selectedQuad);
        if (!selected.success)
        {
            AppendFailure(
                failure,
                RegisterOperationError(
                    "write",
                    "channel-quad selector",
                    Fw2051ScpSelectorRegister,
                    selected.error));
        }
        else
        {
            selectorSelected = true;
            std::this_thread::sleep_for(SelectorSettleTime);
        }
    }

    std::uint16_t currentValue = 0U;
    if (selectorSelected)
    {
        const auto current = readVme(request.registerOffset);
        if (!current.success)
        {
            AppendFailure(
                failure,
                RegisterOperationError(
                    "read",
                    result.settingName,
                    request.registerOffset,
                    current.error));
        }
        else
        {
            currentValue = current.value;
            if (applying)
            {
                result.originalValue = currentValue;
                result.originalCaptured = true;
            }
        }
    }

    if (applying && result.originalCaptured
        && definition->dependencyRule != Fw2051ScpDependencyRule::None)
    {
        result.dependencyRegister = definition->dependencyRegister;
        result.dependencyName = definition->dependencyName;
        const auto dependency = readVme(definition->dependencyRegister);
        if (!dependency.success)
        {
            AppendFailure(
                failure,
                RegisterOperationError(
                    "read",
                    result.dependencyName + " dependency",
                    definition->dependencyRegister,
                    dependency.error));
        }
        else
        {
            result.dependencyChecked = true;
            result.dependencyValue = dependency.value;
            if (!Fw2051ScpDependencySatisfied(
                    *definition,
                    request.requestedValue,
                    result.dependencyValue))
            {
                AppendFailure(
                    failure,
                    "The proposed " + result.settingName + " must be "
                        + Fw2051ScpDependencyRelation(
                            definition->dependencyRule)
                        + " the live " + result.dependencyName
                        + " (proposed "
                        + std::to_string(request.requestedValue)
                        + ", live "
                        + std::to_string(result.dependencyValue)
                        + "); no preview write was sent.");
            }
        }
    }

    if (applying && result.originalCaptured && failure.empty()
        && request.requestedValue == result.originalValue)
    {
        AppendFailure(
            failure,
            "The proposed " + result.settingName
                + " already equals the current value; no write was needed.");
    }
    else if (!applying && selectorSelected
             && currentValue != request.requestedValue)
    {
        AppendFailure(
            failure,
            "The live " + result.settingName
                + " register no longer matches the active preview value; "
                  "restoration was not allowed to overwrite the unexpected value.");
    }

    if (cancellationRequested.load() && failure.empty()
        && !request.automaticallyRestoredOnStop)
    {
        AppendFailure(
            failure,
            "Stop arrived before the parameter write; the pending "
            "transaction was left unchanged for final cleanup.");
    }

    bool recoveryRestoreArmed = false;
    if (applying && failure.empty() && result.originalCaptured)
    {
        std::string journalError;
        recoveryRestoreArmed = SavePreviewRecoveryState(
            session,
            recoveryJournalPath,
            true,
            request,
            result.originalValue,
            journalError);
        if (!recoveryRestoreArmed)
        {
            AppendFailure(
                failure,
                "The crash-recovery restore value could not be journaled, "
                "so no preview write was allowed: " + journalError);
        }
    }

    if (failure.empty())
    {
        const auto targetValue = applying
            ? request.requestedValue
            : request.originalValue;
        result.writeAttempted = applying;
        result.restoreAttempted = !applying;
        const auto written = writeVme(request.registerOffset, targetValue);
        if (!written.success)
        {
            AppendFailure(
                failure,
                RegisterOperationError(
                    applying ? "write" : "restore",
                    result.settingName,
                    request.registerOffset,
                    written.error));
        }
        else
        {
            std::this_thread::sleep_for(FrontendSettleTime);
            const auto readback = readVme(request.registerOffset);
            if (!readback.success)
            {
                AppendFailure(
                    failure,
                    RegisterOperationError(
                        "verify",
                        result.settingName,
                        request.registerOffset,
                        readback.error));
            }
            else if (readback.value != targetValue)
            {
                AppendFailure(
                    failure,
                    result.settingName + " readback was "
                        + std::to_string(readback.value) + ", expected "
                        + std::to_string(targetValue) + ".");
            }
            else if (applying)
            {
                result.appliedReadback = readback.value;
                result.writeVerified = true;
                result.previewActive = true;
            }
            else
            {
                result.restoredReadback = readback.value;
                result.restoreVerified = true;
                result.previewActive = false;
            }
        }
    }

    if (applying && result.writeAttempted && !result.writeVerified
        && result.originalCaptured)
    {
        result.rollbackAttempted = true;
        const auto rolledBack = writeVme(
            request.registerOffset, result.originalValue);
        if (!rolledBack.success)
        {
            AppendFailure(
                failure,
                RegisterOperationError(
                    "rollback",
                    result.settingName,
                    request.registerOffset,
                    rolledBack.error));
        }
        else
        {
            std::this_thread::sleep_for(FrontendSettleTime);
            const auto readback = readVme(request.registerOffset);
            if (!readback.success)
            {
                AppendFailure(
                    failure,
                    RegisterOperationError(
                        "verify rollback",
                        result.settingName,
                        request.registerOffset,
                        readback.error));
            }
            else
            {
                result.restoredReadback = readback.value;
                result.rollbackVerified = readback.value
                    == result.originalValue;
                if (!result.rollbackVerified)
                {
                    AppendFailure(
                        failure,
                        "Rolled-back " + result.settingName
                            + " readback was "
                            + std::to_string(readback.value)
                            + ", expected "
                            + std::to_string(result.originalValue) + ".");
                }
            }
        }
    }

    if (selectorSelected)
    {
        const auto parked = writeVme(Fw2051ScpSelectorRegister, 0U);
        if (!parked.success)
        {
            AppendFailure(
                failure,
                RegisterOperationError(
                    "park",
                    "channel-quad selector",
                    Fw2051ScpSelectorRegister,
                    parked.error));
        }
        else
        {
            result.selectorParkedAtQuadZero = true;
            std::this_thread::sleep_for(SelectorSettleTime);
        }
    }

    const bool configurationSafe = applying
        ? !result.writeAttempted || result.writeVerified
            || result.rollbackVerified
        : !result.restoreAttempted || result.restoreVerified;
    const bool selectorSafe = !selectorSelected
        || result.selectorParkedAtQuadZero;
    const bool previewNowInactive = applying
        ? !result.writeVerified || result.rollbackVerified
        : result.restoreVerified;
    if (previewNowInactive && (recoveryRestoreArmed || !applying))
    {
        std::string journalError;
        if (!SavePreviewRecoveryState(
                session,
                recoveryJournalPath,
                false,
                request,
                result.originalValue,
                journalError))
        {
            AppendFailure(
                failure,
                "The parameter is safe, but the conservative recovery "
                "record could not be updated: " + journalError);
        }
    }

    if (request.resumeAfterTransaction && result.modulePaused
        && result.daqModePaused && configurationSafe && selectorSafe)
    {
        const auto resumed = ResumeDiagnosticDataTaking(
            transport,
            session.acquisition.baseAddress,
            session.nextSuperReference,
            session.nextStackReference,
            cancellationRequested);
        result.fifoResetSent = resumed.fifoResetSent;
        result.readoutResetSent = resumed.readoutResetSent;
        result.acquisitionResumed = resumed.acquisitionResumed;
        result.daqModeReadbackValid = resumed.daqModeReadbackValid;
        result.daqModeReadback = resumed.daqModeReadback;
        result.daqModeResumed = resumed.daqModeResumed;
        AppendFailure(failure, resumed.error);
    }

    const auto duration = std::chrono::duration_cast<
        std::chrono::microseconds>(
        std::chrono::steady_clock::now() - startedAt);
    if (applying)
    {
        result.applyDurationMicroseconds =
            static_cast<std::uint64_t>(duration.count());
    }
    else
    {
        result.restoreDurationMicroseconds =
            static_cast<std::uint64_t>(duration.count());
    }

    const bool resumeSafe = !request.resumeAfterTransaction
        || (result.acquisitionResumed && result.daqModeResumed);
    const bool transactionSafe = configurationSafe && selectorSafe
        && resumeSafe;
    const bool transactionPassed = failure.empty() && selectorSafe
        && resumeSafe
        && (applying ? result.writeVerified : result.restoreVerified);
    if (transactionPassed)
    {
        result.state = applying
            ? DiagnosticParameterPreviewState::PreviewActive
            : DiagnosticParameterPreviewState::Restored;
        result.message = applying
            ? result.settingName
                + " preview applied and verified; acquisition resumed. "
                  "Restore remains available."
            : request.automaticallyRestoredOnStop
                ? "Original " + result.settingName
                    + " automatically restored and verified before tuner cleanup."
                : "Original " + result.settingName
                    + " restored and verified; acquisition resumed.";
        return result;
    }

    result.state = DiagnosticParameterPreviewState::Failed;
    result.message = transactionSafe && !request.automaticallyRestoredOnStop
        ? failure.empty()
            ? "The parameter transaction made no change, but the acquisition state remains safe."
            : failure
                + " The register is unchanged or was recovered, and the acquisition state remains safe."
        : failure.empty()
            ? "The parameter transaction could not recover a verified safe state."
            : failure;
    return result;
}

} // namespace

DiagnosticParameterPreviewResult ApplyDiagnosticParameterPreview(
    ICommandTransport& transport,
    DiagnosticAcquisitionPreparationResult& acquisitionSession,
    const DiagnosticParameterPreviewRequest& request,
    const std::string& recoveryJournalPath,
    const std::atomic<bool>& cancellationRequested)
{
    return ExecutePreviewTransaction(
        transport,
        acquisitionSession,
        {
            PreviewAction::Apply,
            request.selectedQuad,
            request.registerOffset,
            request.requestedValue,
            0U,
            true,
            false,
        },
        recoveryJournalPath,
        cancellationRequested);
}

DiagnosticParameterPreviewResult RestoreDiagnosticParameterPreview(
    ICommandTransport& transport,
    DiagnosticAcquisitionPreparationResult& acquisitionSession,
    const DiagnosticParameterPreviewResult& activePreview,
    const std::string& recoveryJournalPath,
    const bool resumeAfterTransaction,
    const bool automaticallyRestoredOnStop,
    const std::atomic<bool>& cancellationRequested)
{
    if (!activePreview.previewActive
        || activePreview.state
            != DiagnosticParameterPreviewState::PreviewActive)
    {
        auto result = activePreview;
        result.state = DiagnosticParameterPreviewState::Failed;
        result.message = "There is no active parameter preview to restore.";
        return result;
    }
    return ExecutePreviewTransaction(
        transport,
        acquisitionSession,
        {
            PreviewAction::Restore,
            activePreview.selectedQuad,
            activePreview.registerOffset,
            activePreview.requestedValue,
            activePreview.originalValue,
            resumeAfterTransaction,
            automaticallyRestoredOnStop,
        },
        recoveryJournalPath,
        cancellationRequested);
}

} // namespace fidget
