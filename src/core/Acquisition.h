#ifndef FIDGET_CORE_ACQUISITION_H
#define FIDGET_CORE_ACQUISITION_H

#include "core/StreamDecoder.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fidget {

enum class DiagnosticAcquisitionState
{
    NotRun,
    Starting,
    Running,
    Stopping,
    Stopped,
    Failed,
};

enum class DiagnosticFingerprintOutcome
{
    Verified,
    CommunicationUnavailable,
    ForeignFingerprint,
};

enum class DiagnosticSourceChangeState
{
    NotRun,
    Applying,
    Passed,
    Failed,
};

struct DiagnosticSourceChangeResult
{
    DiagnosticSourceChangeState state = DiagnosticSourceChangeState::NotRun;
    std::string message = "No waveform-source change has been requested";
    std::uint16_t selectedQuad = 0U;
    std::uint8_t requestedSource = 0U;
    std::uint16_t originalConfiguration = 0U;
    std::uint16_t requestedConfiguration = 0U;
    std::uint16_t appliedReadback = 0U;
    std::uint16_t restoredReadback = 0U;
    std::uint32_t daqModeReadback = 0U;
    bool fingerprintVerified = false;
    bool foreignFingerprint = false;
    bool communicationUnavailable = false;
    bool originalCaptured = false;
    bool modulePaused = false;
    bool daqModePaused = false;
    bool writeAttempted = false;
    bool writeVerified = false;
    bool rollbackAttempted = false;
    bool rollbackVerified = false;
    bool selectorParkedAtQuadZero = false;
    bool fifoResetSent = false;
    bool readoutResetSent = false;
    bool acquisitionResumed = false;
    bool daqModeReadbackValid = false;
    bool daqModeResumed = false;
};

enum class DiagnosticParameterPreviewState
{
    NotRun,
    Applying,
    PreviewActive,
    Restoring,
    Restored,
    Failed,
};

struct DiagnosticParameterPreviewResult
{
    DiagnosticParameterPreviewState state =
        DiagnosticParameterPreviewState::NotRun;
    std::string message = "No parameter preview has been requested";
    std::uint16_t selectedQuad = 0U;
    std::uint16_t registerOffset = 0U;
    std::string settingName;
    std::uint16_t originalValue = 0U;
    std::uint16_t requestedValue = 0U;
    std::uint16_t appliedReadback = 0U;
    std::uint16_t restoredReadback = 0U;
    std::uint16_t dependencyRegister = 0U;
    std::string dependencyName;
    std::uint16_t dependencyValue = 0U;
    std::uint32_t daqModeReadback = 0U;
    std::uint64_t applyDurationMicroseconds = 0U;
    std::uint64_t restoreDurationMicroseconds = 0U;
    bool fingerprintVerified = false;
    bool foreignFingerprint = false;
    bool communicationUnavailable = false;
    bool originalCaptured = false;
    bool modulePaused = false;
    bool daqModePaused = false;
    bool writeAttempted = false;
    bool writeVerified = false;
    bool rollbackAttempted = false;
    bool rollbackVerified = false;
    bool selectorParkedAtQuadZero = false;
    bool fifoResetSent = false;
    bool readoutResetSent = false;
    bool acquisitionResumed = false;
    bool daqModeReadbackValid = false;
    bool daqModeResumed = false;
    bool previewActive = false;
    bool restoreAttempted = false;
    bool restoreVerified = false;
    bool automaticallyRestoredOnStop = false;
    bool dependencyChecked = false;
};

struct DiagnosticFingerprintResult
{
    DiagnosticFingerprintOutcome outcome =
        DiagnosticFingerprintOutcome::CommunicationUnavailable;
    std::uint32_t daqMode = 0U;
    std::string message;
};

struct DiagnosticModuleIsolation
{
    std::uint32_t baseAddress = 0U;
    std::uint16_t hardwareId = 0U;
    std::uint16_t irqLevel = 0U;
    std::uint16_t acquisitionStateBefore = 0U;
    std::uint16_t acquisitionStateAfter = 0xFFFFU;
    bool validated = false;

    // This is diagnostic only. Every configured non-target is isolated,
    // including a module with IRQ disabled or a different IRQ level.
    bool sharesTargetIrq = false;
    bool stopRequired = false;
    bool quiescenceAttempted = false;
    bool stopSent = false;
    bool stopVerified = false;
    bool fifoResetSent = false;
    bool readoutResetSent = false;
    bool cleanupVerified = false;
    std::string message;
};

struct DiagnosticAcquisitionResult
{
    DiagnosticAcquisitionState state = DiagnosticAcquisitionState::NotRun;
    std::string message = "No direct diagnostic acquisition has been run";
    std::uint32_t baseAddress = 0U;
    std::uint16_t hardwareId = 0U;
    std::uint16_t outputFormat = 0U;
    std::uint16_t irqLevel = 0U;
    std::uint16_t requestedChannel = 0U;
    std::uint16_t dataPort = 0U;
    std::uint64_t datagramsReceived = 0U;
    std::uint64_t bytesReceived = 0U;
    std::uint64_t ownershipHeartbeatChecks = 0U;
    bool moduleStopSent = false;
    bool daqModeDisabled = false;
    bool readoutStackDisabled = false;
    bool foreignControllerDetected = false;
    bool cleanupSkippedToProtectForeignRun = false;
    bool communicationUncertain = false;
    std::uint64_t commandPathFailures = 0U;
    std::uint64_t commandPathRecoveries = 0U;
    bool orphanRecoveryRequired = false;
    bool recoveryJournalPrepared = false;
    bool recoveryJournalActive = false;
    bool recoveryJournalRemoved = false;
    std::size_t configuredModuleCount = 0U;
    std::size_t nonTargetModuleCount = 0U;
    std::size_t activeNonTargetModulesFound = 0U;
    std::size_t nonTargetModulesQuiesced = 0U;
    std::size_t nonTargetModulesVerifiedStoppedOnCleanup = 0U;
    std::vector<DiagnosticModuleIsolation> moduleIsolation;
};

struct DiagnosticStreamSnapshot
{
    bool receiverRunning = false;
    std::string receiverError;
    std::uint16_t requestedChannel = 0U;
    MdppRequestedChannelTarget requestedTarget;
    MdppChannelHistorySnapshots histories;
    StreamDecoderStats decoderStats;
    struct ChannelWaveformTotal
    {
        std::uint16_t channel = 0U;
        std::uint64_t total = 0U;
    };
    std::vector<ChannelWaveformTotal> channelWaveformTotals;
    std::uint64_t datagramsReceived = 0U;
    std::uint64_t bytesReceived = 0U;
    double datagramsPerSecond = 0.0;
    double waveformsPerSecond = 0.0;
};

} // namespace fidget

#endif
