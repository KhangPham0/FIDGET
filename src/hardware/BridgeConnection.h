#ifndef FIDGET_HARDWARE_BRIDGE_CONNECTION_H
#define FIDGET_HARDWARE_BRIDGE_CONNECTION_H

#include "core/BridgeProtocol.h"
#include "hardware/Transport.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fidget {

class BridgeConnection
{
public:
    static constexpr std::size_t DefaultQueueCapacity = 256U;

    BridgeConnection(
        int readDescriptor,
        int writeDescriptor,
        std::chrono::milliseconds handshakeTimeout =
            std::chrono::seconds(10),
        std::size_t queueCapacity = DefaultQueueCapacity);
    ~BridgeConnection();

    BridgeConnection(const BridgeConnection&) = delete;
    BridgeConnection& operator=(const BridgeConnection&) = delete;

    [[nodiscard]] TransportOperationResult OpenChannel(
        BridgeChannel channel,
        const std::string& host,
        std::uint16_t port);
    [[nodiscard]] TransportOperationResult Send(
        BridgeChannel channel,
        const std::byte* data,
        std::size_t size);
    [[nodiscard]] TransportReceiveResult Receive(
        BridgeChannel channel,
        std::byte* buffer,
        std::size_t capacity,
        int timeoutMilliseconds);

    void CloseChannel(BridgeChannel channel) noexcept;
    void Close() noexcept;

private:
    using Datagram = std::vector<std::byte>;
    using DatagramQueue = std::deque<Datagram>;

    [[nodiscard]] bool ValidateEndpointLocked(
        BridgeChannel channel,
        const std::string& host,
        std::uint16_t port,
        std::string& error);
    [[nodiscard]] bool ChannelOpenLocked(BridgeChannel channel) const;
    void SetChannelOpenLocked(BridgeChannel channel, bool open);
    [[nodiscard]] DatagramQueue& QueueLocked(BridgeChannel channel);
    [[nodiscard]] const char* ClosedError(BridgeChannel channel) const;
    [[nodiscard]] const char* ReceiveClosedError(
        BridgeChannel channel) const;
    [[nodiscard]] const char* InvalidTimeoutError(
        BridgeChannel channel) const;
    [[nodiscard]] const char* TimeoutError(BridgeChannel channel) const;

    [[nodiscard]] TransportOperationResult WriteFrame(
        const std::vector<std::byte>& frameBytes);
    void ReadLoop();
    [[nodiscard]] bool RouteFrames(std::vector<BridgeFrame> frames);
    void FailLocked(std::string error);
    void StopAndJoin() noexcept;
    void CloseReadDescriptor() noexcept;
    void CloseWriteDescriptor() noexcept;

    int readDescriptor_ = -1;
    int writeDescriptor_ = -1;
    const std::chrono::milliseconds handshakeTimeout_;
    const std::size_t queueCapacity_;

    std::mutex openMutex_;
    std::mutex shutdownMutex_;
    std::mutex writeMutex_;
    std::mutex stateMutex_;
    std::condition_variable stateChanged_;
    DatagramQueue commandQueue_;
    DatagramQueue dataQueue_;
    std::string endpointHost_;
    std::uint16_t commandPort_ = 0U;
    bool endpointSet_ = false;
    bool commandOpen_ = false;
    bool dataOpen_ = false;
    bool readerStarted_ = false;
    bool handshakeComplete_ = false;
    bool stopRequested_ = false;
    bool closed_ = false;
    std::string failure_;

    // Keep the thread last so every object it uses is initialized first.
    std::thread readerThread_;
};

} // namespace fidget

#endif
