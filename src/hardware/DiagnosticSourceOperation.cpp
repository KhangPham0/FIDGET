#include "hardware/DiagnosticSourceOperation.h"

#include "core/ScpConfiguration.h"
#include "hardware/DiagnosticPauseResume.h"
#include "hardware/VmeTransaction.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>

namespace fidget {
namespace {

constexpr std::uint16_t SampleConfigurationRegister = 0x614AU;
constexpr auto SelectorSettleTime = std::chrono::microseconds(50);
constexpr auto FrontendSettleTime = std::chrono::microseconds(20);

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

} // namespace

DiagnosticSourceChangeResult ChangeDiagnosticWaveformSource(
    ICommandTransport& transport,
    DiagnosticAcquisitionPreparationResult& acquisitionSession,
    const DiagnosticSourceChangeRequest& request,
    const std::atomic<bool>& cancellationRequested)
{
    DiagnosticSourceChangeResult result;
    result.state = DiagnosticSourceChangeState::Applying;
    result.selectedQuad = request.selectedQuad;
    result.requestedSource = request.requestedSource;
    result.message =
        "Pausing direct acquisition and applying the waveform source...";

    if (request.selectedQuad >= Fw2051ScpQuadCount)
    {
        result.state = DiagnosticSourceChangeState::Failed;
        result.message = "The SCP channel quad must be between 0 and 7.";
        return result;
    }
    if (request.requestedSource > 3U)
    {
        result.state = DiagnosticSourceChangeState::Failed;
        result.message = "The waveform source must be between 0 and 3.";
        return result;
    }
    if (acquisitionSession.acquisition.state
        != DiagnosticAcquisitionState::Running)
    {
        result.state = DiagnosticSourceChangeState::Failed;
        result.message =
            "Waveform source can change only while direct acquisition is running.";
        return result;
    }

    auto fingerprintReference = acquisitionSession.nextSuperReference;
    const auto fingerprint = VerifyDiagnosticOwnershipFingerprint(
        transport,
        acquisitionSession,
        fingerprintReference,
        cancellationRequested);
    acquisitionSession.nextSuperReference = fingerprintReference;
    if (fingerprint.outcome != DiagnosticFingerprintOutcome::Verified)
    {
        result.foreignFingerprint = fingerprint.outcome
            == DiagnosticFingerprintOutcome::ForeignFingerprint;
        result.communicationUnavailable = fingerprint.outcome
            == DiagnosticFingerprintOutcome::CommunicationUnavailable;
        result.state = DiagnosticSourceChangeState::Failed;
        result.message = fingerprint.message;
        return result;
    }
    result.fingerprintVerified = true;

    std::string failure;
    const auto paused = PauseDiagnosticDataTaking(
        transport,
        acquisitionSession.acquisition.baseAddress,
        acquisitionSession.nextSuperReference,
        acquisitionSession.nextStackReference,
        cancellationRequested);
    result.modulePaused = paused.modulePaused;
    result.daqModePaused = paused.daqModePaused;
    AppendFailure(failure, paused.error);

    bool selectorSelected = false;
    const auto writeVme = [&](const std::uint16_t registerOffset,
                              const std::uint16_t value) {
        return WriteVmeD16(
            transport,
            acquisitionSession.acquisition.baseAddress + registerOffset,
            value,
            acquisitionSession.nextSuperReference,
            acquisitionSession.nextStackReference,
            cancellationRequested);
    };
    const auto readVme = [&](const std::uint16_t registerOffset) {
        return ReadVmeD16(
            transport,
            acquisitionSession.acquisition.baseAddress + registerOffset,
            acquisitionSession.nextSuperReference,
            acquisitionSession.nextStackReference,
            cancellationRequested);
    };

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

    if (selectorSelected)
    {
        const auto original = readVme(SampleConfigurationRegister);
        if (!original.success)
        {
            AppendFailure(
                failure,
                RegisterOperationError(
                    "read",
                    "sample configuration",
                    SampleConfigurationRegister,
                    original.error));
        }
        else
        {
            result.originalCaptured = true;
            result.originalConfiguration = original.value;
            result.requestedConfiguration = static_cast<std::uint16_t>(
                (result.originalConfiguration & 0xFFFCU)
                | request.requestedSource);
        }
    }

    if (result.originalCaptured && cancellationRequested.load())
    {
        AppendFailure(
            failure,
            "The Stop request arrived before the source write; the "
            "captured configuration was left unchanged.");
    }
    else if (result.originalCaptured)
    {
        if (result.requestedConfiguration == result.originalConfiguration)
        {
            result.appliedReadback = result.originalConfiguration;
            result.writeVerified = true;
        }
        else
        {
            result.writeAttempted = true;
            const auto written = writeVme(
                SampleConfigurationRegister,
                result.requestedConfiguration);
            if (!written.success)
            {
                AppendFailure(
                    failure,
                    RegisterOperationError(
                        "write",
                        "sample configuration",
                        SampleConfigurationRegister,
                        written.error));
            }
            else
            {
                std::this_thread::sleep_for(FrontendSettleTime);
                const auto readback = readVme(SampleConfigurationRegister);
                if (!readback.success)
                {
                    AppendFailure(
                        failure,
                        RegisterOperationError(
                            "verify",
                            "sample configuration",
                            SampleConfigurationRegister,
                            readback.error));
                }
                else
                {
                    result.appliedReadback = readback.value;
                    if (result.appliedReadback
                        == result.requestedConfiguration)
                    {
                        result.writeVerified = true;
                    }
                    else
                    {
                        AppendFailure(
                            failure,
                            "Sample-configuration readback was "
                                + std::to_string(result.appliedReadback)
                                + ", expected "
                                + std::to_string(
                                    result.requestedConfiguration)
                                + ".");
                    }
                }
            }
        }
    }

    if (result.writeAttempted && !result.writeVerified
        && result.originalCaptured)
    {
        result.rollbackAttempted = true;
        const auto restored = writeVme(
            SampleConfigurationRegister,
            result.originalConfiguration);
        if (!restored.success)
        {
            AppendFailure(
                failure,
                RegisterOperationError(
                    "restore",
                    "sample configuration",
                    SampleConfigurationRegister,
                    restored.error));
        }
        else
        {
            std::this_thread::sleep_for(FrontendSettleTime);
            const auto readback = readVme(SampleConfigurationRegister);
            if (!readback.success)
            {
                AppendFailure(
                    failure,
                    RegisterOperationError(
                        "verify restored",
                        "sample configuration",
                        SampleConfigurationRegister,
                        readback.error));
            }
            else
            {
                result.restoredReadback = readback.value;
                result.rollbackVerified = result.restoredReadback
                    == result.originalConfiguration;
                if (!result.rollbackVerified)
                {
                    AppendFailure(
                        failure,
                        "Restored sample-configuration readback was "
                            + std::to_string(result.restoredReadback)
                            + ", expected "
                            + std::to_string(result.originalConfiguration)
                            + ".");
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

    const bool configurationSafe = !result.writeAttempted
        || result.writeVerified || result.rollbackVerified;
    const bool selectorSafe = !selectorSelected
        || result.selectorParkedAtQuadZero;
    if (result.modulePaused && result.daqModePaused
        && configurationSafe && selectorSafe)
    {
        const auto resumed = ResumeDiagnosticDataTaking(
            transport,
            acquisitionSession.acquisition.baseAddress,
            acquisitionSession.nextSuperReference,
            acquisitionSession.nextStackReference,
            cancellationRequested);
        result.fifoResetSent = resumed.fifoResetSent;
        result.readoutResetSent = resumed.readoutResetSent;
        result.acquisitionResumed = resumed.acquisitionResumed;
        result.daqModeReadbackValid = resumed.daqModeReadbackValid;
        result.daqModeReadback = resumed.daqModeReadback;
        result.daqModeResumed = resumed.daqModeResumed;
        AppendFailure(failure, resumed.error);
    }

    const bool resumedSafely = result.acquisitionResumed
        && result.daqModeResumed;
    const bool stoppedByUser = cancellationRequested.load();
    const bool passed = failure.empty() && result.originalCaptured
        && result.writeVerified && result.selectorParkedAtQuadZero
        && resumedSafely;
    const bool recoveredSafely = configurationSafe && selectorSafe
        && resumedSafely;
    result.state = passed
        ? DiagnosticSourceChangeState::Passed
        : DiagnosticSourceChangeState::Failed;
    if (passed)
    {
        result.message = "Waveform source "
            + std::to_string(request.requestedSource) + " applied to quad "
            + std::to_string(request.selectedQuad)
            + "; acquisition resumed and non-source configuration bits "
              "were preserved.";
    }
    else if (stoppedByUser && configurationSafe && selectorSafe)
    {
        result.message =
            "Waveform-source transaction was interrupted by the Stop "
            "request; final acquisition cleanup is running.";
    }
    else if (recoveredSafely)
    {
        result.message = failure.empty()
            ? "The source was not changed, but acquisition resumed safely."
            : failure
                + " The original setting is intact or was restored, and "
                  "acquisition resumed.";
    }
    else
    {
        result.message = failure.empty()
            ? "The waveform-source transaction could not recover a verified running state."
            : failure;
    }
    return result;
}

} // namespace fidget
