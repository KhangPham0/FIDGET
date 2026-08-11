#ifndef FIDGET_HARDWARE_ACQUISITION_RECEIVER_H
#define FIDGET_HARDWARE_ACQUISITION_RECEIVER_H

#include "core/Acquisition.h"
#include "core/StreamDecoder.h"
#include "hardware/Transport.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace fidget {

using DiagnosticStreamPublisher =
    std::function<void(const DiagnosticStreamSnapshot& snapshot)>;

class AcquisitionReceiver
{
public:
    explicit AcquisitionReceiver(
        IDataReceiver& receiver,
        std::chrono::milliseconds publishInterval =
            std::chrono::milliseconds(200));
    ~AcquisitionReceiver();

    AcquisitionReceiver(const AcquisitionReceiver&) = delete;
    AcquisitionReceiver& operator=(const AcquisitionReceiver&) = delete;

    [[nodiscard]] bool Start(
        std::uint16_t requestedChannel,
        DiagnosticStreamPublisher publisher);
    void StopAndJoin();

    [[nodiscard]] DiagnosticStreamSnapshot CurrentSnapshot() const;
    [[nodiscard]] bool IsRunning() const noexcept;

private:
    void Run();
    void Publish(bool running, std::string receiverError = {});

    IDataReceiver& receiver_;
    const std::chrono::milliseconds publishInterval_;
    MvmeStreamDecoder decoder_;
    DiagnosticStreamPublisher publisher_;
    mutable std::mutex snapshotMutex_;
    DiagnosticStreamSnapshot snapshot_;
    std::atomic<bool> stopRequested_{true};
    std::atomic<bool> running_{false};
    std::uint64_t datagramsReceived_ = 0U;
    std::uint64_t bytesReceived_ = 0U;
    int observedModuleId_ = -1;
    std::thread receiverThread_;
};

} // namespace fidget

#endif
