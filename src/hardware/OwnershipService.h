#ifndef FIDGET_HARDWARE_OWNERSHIP_SERVICE_H
#define FIDGET_HARDWARE_OWNERSHIP_SERVICE_H

#include "core/TunerControl.h"
#include "hardware/CommandWorker.h"
#include "hardware/ScpCaptureOperation.h"
#include "hardware/Transport.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace fidget {

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
        std::unique_ptr<ICommandTransport> transport,
        std::chrono::milliseconds watchdogInterval =
            std::chrono::seconds(1));
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

    void StartWatchdog();
    void StopWatchdog();
    void RequestWatchdogStop();
    void WatchdogLoop();
    void PollWatchdog();
    void DetachForForeignDaq(std::uint32_t daqMode, std::string message);

    void Publish(TunerSnapshot snapshot);
    void PublishStatus(
        TunerSnapshot snapshot,
        TunerStatusLevel level,
        std::string summary,
        std::string detail = {});

    std::unique_ptr<ICommandTransport> transport_;
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
};

} // namespace fidget

#endif
