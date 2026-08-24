#ifndef FIDGET_HARDWARE_OWNERSHIP_SERVICE_H
#define FIDGET_HARDWARE_OWNERSHIP_SERVICE_H

#include "core/ApplicationStorage.h"
#include "core/TunerControl.h"
#include "hardware/AcquisitionReceiver.h"
#include "hardware/CommandWorker.h"
#include "hardware/DeterministicStartupOperation.h"
#include "hardware/DiagnosticAcquisitionOperation.h"
#include "hardware/DiagnosticPreviewOperation.h"
#include "hardware/DiagnosticRecoveryOperation.h"
#include "hardware/DiagnosticSourceOperation.h"
#include "hardware/ScpCaptureOperation.h"
#include "hardware/ScpBulkApplyOperation.h"
#include "hardware/ScpSingleRepairOperation.h"
#include "hardware/Transport.h"
#include "hardware/TransportFactory.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace fidget {

class TuningSessionCoordinator;

inline constexpr std::uint16_t HardwareIdRegister = 0x6008U;
inline constexpr std::uint16_t FirmwareRevisionRegister = 0x600EU;
inline constexpr std::uint16_t DaqModeRegister = 0x1300U;
inline constexpr std::uint32_t ExpectedMvlcHardwareId = 0x5008U;

struct PreWriteGateResult
{
    bool allowed = false;
    std::string message;
};

class OwnershipService final : public ITunerControl
{
public:
    explicit OwnershipService(
        std::unique_ptr<ITransportFactory> transportFactory,
        std::chrono::milliseconds watchdogInterval =
            std::chrono::seconds(1),
        ApplicationStoragePaths applicationStoragePaths =
            DefaultApplicationStoragePaths());
    ~OwnershipService() override;

    OwnershipService(const OwnershipService&) = delete;
    OwnershipService& operator=(const OwnershipService&) = delete;

    [[nodiscard]] std::shared_ptr<const TunerSnapshot>
        CurrentSnapshot() const override;
    void Submit(TunerCommand command) override;

    [[nodiscard]] std::future<PreWriteGateResult> VerifyPreWriteGate(
        std::string operationName);

private:
    struct LocalReadResult
    {
        bool success = false;
        std::uint32_t value = 0U;
        std::string error;
    };

    void UseProject(UseCrateProjectCommand command);
    void ClearProject();
    void CheckStatus();
    void SetHandoffConfirmed(bool confirmed);
    void OpenSession();
    void RunStartupAudit();
    void CaptureConfiguration();
    void SaveProfile(const std::string& path);
    void LoadProfile(const std::string& path);
    void ExportMvmeScript(const ExportMvmeScriptCommand& command);
    void ApplyProfileRow(const ApplyProfileRowCommand& command);
    void ApplyAllDifferences();
    void RunDeterministicStartup(
        const RunDeterministicStartupCommand& command);
    void StartDiagnosticAcquisition(
        const StartDiagnosticAcquisitionCommand& command);
    void ChangeDiagnosticSource(
        const ChangeDiagnosticSourceCommand& command);
    void ApplyDiagnosticPreview(
        const ApplyDiagnosticPreviewCommand& command);
    void RecoverDiagnosticOrphan(
        const RecoverDiagnosticOrphanCommand& command);
    void CheckDiagnosticRecoveryStatus();
    [[nodiscard]] bool RestoreDiagnosticPreview(
        bool resumeAfterTransaction,
        bool automaticallyRestoredOnStop);
    [[nodiscard]] bool RestoreDiagnosticSource(
        bool ownershipAlreadyVerifiedAndPaused);
    [[nodiscard]] bool StopDiagnosticAcquisition();
    void PublishDiagnosticStream(DiagnosticStreamSnapshot stream);
    void ReleaseSession();
    void ProbeController(TunerSnapshot snapshot, bool retainSession);

    [[nodiscard]] LocalReadResult ReadLocalRegister(
        std::uint16_t address,
        std::uint16_t reference,
        const std::atomic<bool>& cancelled);
    [[nodiscard]] PreWriteGateResult CheckPreWriteGate(
        const std::string& operationName);
    [[nodiscard]] ScpCaptureGateResult CheckCaptureOwnershipGate(
        const std::string& operationName);
    [[nodiscard]] ScpCaptureGateResult CheckApplyOwnershipGate(
        const std::string& operationName);
    [[nodiscard]] ScpCaptureGateResult CheckStartupOwnershipGate(
        const std::string& operationName);
    static void RefreshProfileComparison(TunerSnapshot& snapshot);

    void StartWatchdog();
    void StopWatchdog();
    void RequestWatchdogStop();
    void WatchdogLoop();
    void PollWatchdog();
    void DetachForForeignDaq(std::uint32_t daqMode, std::string message);
    void DetachForForeignDiagnosticFingerprint(
        TunerSnapshot snapshot,
        std::string message);

    void Publish(TunerSnapshot snapshot);
    void PublishStatus(
        TunerSnapshot snapshot,
        TunerStatusLevel level,
        std::string summary,
        std::string detail = {});
    void PublishActivityStatus(
        TunerSnapshot snapshot,
        ActivityLogCategory category,
        TunerStatusLevel level,
        std::string summary,
        std::string detail = {},
        std::optional<ActivityParameterChange> parameterChange =
            std::nullopt);
    void AppendActivity(
        TunerSnapshot& snapshot,
        ActivityLogCategory category,
        TunerStatusLevel level,
        std::string summary,
        std::optional<ActivityParameterChange> parameterChange =
            std::nullopt);
    void AppendBulkWriteActivities(
        TunerSnapshot& snapshot,
        const ScpBulkApplyResult& result);

    [[nodiscard]] bool CreateTransportSession(
        const CrateProject& project,
        std::string& error);
    [[nodiscard]] std::string ResetTransportSession() noexcept;

    std::unique_ptr<ITransportFactory> transportFactory_;
    std::unique_ptr<TransportSession> transportSession_;
    ICommandTransport* transport_ = nullptr;
    IDataReceiver* dataReceiver_ = nullptr;
    std::unique_ptr<AcquisitionReceiver> acquisitionReceiver_;
    std::optional<DiagnosticAcquisitionPreparationResult>
        acquisitionSession_;
    DiagnosticAcquisitionPreparationRequest acquisitionRequest_;
    std::string recoveryJournalPath_;
    std::string activityLogPath_;
    std::optional<TunerRecoveryRecord> pendingRecoveryRecord_;
    CommandWorker worker_;
    std::shared_ptr<const TunerSnapshot> snapshot_;
    CrateProject project_;
    std::size_t activeModuleIndex_ = 0U;
    std::uint16_t nextReadReference_ = 1U;
    std::uint16_t nextWatchdogReference_ = 0x7000U;
    std::uint16_t nextAuditSuperReference_ = 0x1600U;
    std::uint32_t nextAuditStackReference_ = 0x9C080001U;
    bool watchdogCommunicationUncertain_ = false;
    const std::chrono::milliseconds watchdogInterval_;
    std::atomic<bool> serviceStopRequested_{false};
    std::atomic<bool> watchdogStopRequested_{true};
    std::mutex watchdogMutex_;
    std::condition_variable watchdogWakeup_;
    std::thread watchdog_;
    std::unique_ptr<TuningSessionCoordinator> tuningSessionCoordinator_;
};

} // namespace fidget

#endif
