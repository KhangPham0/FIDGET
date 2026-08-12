#include "hardware/AcquisitionReceiver.h"

#include "hardware/DiagnosticAcquisitionOperation.h"
#include "hardware/VmeTransaction.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <string>
#include <utility>

namespace fidget {

AcquisitionReceiver::AcquisitionReceiver(
    IDataReceiver& receiver,
    const std::chrono::milliseconds publishInterval)
    : receiver_(receiver)
    , publishInterval_(publishInterval)
{
}

AcquisitionReceiver::~AcquisitionReceiver()
{
    StopAndJoin();
}

bool AcquisitionReceiver::Start(
    const std::uint16_t requestedChannel,
    DiagnosticStreamPublisher publisher)
{
    if (requestedChannel > 31U || !publisher || receiverThread_.joinable())
    {
        return false;
    }

    decoder_.Reset();
    publisher_ = std::move(publisher);
    datagramsReceived_ = 0U;
    bytesReceived_ = 0U;
    observedModuleId_ = -1;
    {
        const std::lock_guard<std::mutex> lock(snapshotMutex_);
        snapshot_ = {};
        snapshot_.requestedChannel = requestedChannel;
        snapshot_.requestedTarget.requestedChannel = requestedChannel;
    }
    stopRequested_.store(false);
    running_.store(true);
    receiverThread_ = std::thread(&AcquisitionReceiver::Run, this);
    return true;
}

void AcquisitionReceiver::StopAndJoin()
{
    stopRequested_.store(true);
    if (receiverThread_.joinable())
    {
        receiverThread_.join();
    }
}

DiagnosticStreamSnapshot AcquisitionReceiver::CurrentSnapshot() const
{
    const std::lock_guard<std::mutex> lock(snapshotMutex_);
    return snapshot_;
}

bool AcquisitionReceiver::IsRunning() const noexcept
{
    return running_.load();
}

void AcquisitionReceiver::Run()
{
    std::array<std::byte, MvlcCommandResponseBufferSize> receiveBuffer{};
    auto lastPublish = std::chrono::steady_clock::now();
    auto lastRateTime = lastPublish;
    std::uint64_t lastRateDatagrams = 0U;
    std::uint64_t lastRateWaveforms = 0U;
    double datagramRate = 0.0;
    double waveformRate = 0.0;

    while (!stopRequested_.load())
    {
        const auto received = receiver_.Receive(
            receiveBuffer.data(),
            receiveBuffer.size(),
            DiagnosticDataReceiveTimeoutMilliseconds);
        if (received.status == TransportReceiveStatus::Received)
        {
            decoder_.Consume(receiveBuffer.data(), received.bytesReceived);
            ++datagramsReceived_;
            bytesReceived_ += received.bytesReceived;
        }
        else if (received.status == TransportReceiveStatus::Error)
        {
            Publish(false, received.error);
            running_.store(false);
            return;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - lastRateTime >= std::chrono::seconds(1))
        {
            const double elapsed =
                std::chrono::duration<double>(now - lastRateTime).count();
            const auto stats = decoder_.Stats();
            datagramRate = static_cast<double>(
                datagramsReceived_ - lastRateDatagrams) / elapsed;
            waveformRate = static_cast<double>(
                stats.decodedWaveforms - lastRateWaveforms) / elapsed;
            lastRateDatagrams = datagramsReceived_;
            lastRateWaveforms = stats.decodedWaveforms;
            lastRateTime = now;
        }
        if (now - lastPublish >= publishInterval_)
        {
            {
                const std::lock_guard<std::mutex> lock(snapshotMutex_);
                snapshot_.datagramsPerSecond = datagramRate;
                snapshot_.waveformsPerSecond = waveformRate;
            }
            Publish(true);
            lastPublish = now;
        }
    }

    Publish(false);
    running_.store(false);
}

void AcquisitionReceiver::Publish(
    const bool running,
    std::string receiverError)
{
    auto channels = decoder_.AvailableChannels();
    DiagnosticStreamSnapshot next;
    {
        const std::lock_guard<std::mutex> lock(snapshotMutex_);
        next = snapshot_;
    }
    next.receiverRunning = running;
    next.receiverError = std::move(receiverError);
    next.histories = decoder_.AllHistories();
    next.decoderStats = decoder_.Stats();
    std::array<std::uint64_t, 32> totalsByPhysicalChannel{};
    for (const auto& entry : next.histories)
    {
        const int channel = entry.first.channel;
        if (channel >= 0 && channel < 32)
        {
            totalsByPhysicalChannel[static_cast<std::size_t>(channel)]
                += entry.second.totalCaptured;
        }
    }
    next.channelWaveformTotals.clear();
    for (std::size_t channel = 0U;
         channel < totalsByPhysicalChannel.size();
         ++channel)
    {
        if (totalsByPhysicalChannel[channel] == 0U)
        {
            continue;
        }
        next.channelWaveformTotals.push_back({
            static_cast<std::uint16_t>(channel),
            totalsByPhysicalChannel[channel],
        });
    }
    next.datagramsReceived = datagramsReceived_;
    next.bytesReceived = bytesReceived_;
    next.requestedTarget = ResolveMdppRequestedChannelTarget(
        channels.data(),
        channels.size(),
        observedModuleId_,
        next.requestedChannel);
    if (next.requestedTarget.moduleObserved)
    {
        observedModuleId_ = next.requestedTarget.moduleId;
    }

    {
        const std::lock_guard<std::mutex> lock(snapshotMutex_);
        snapshot_ = next;
    }
    publisher_(next);
}

} // namespace fidget
