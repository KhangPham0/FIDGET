#include "hardware/BridgeConnection.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <exception>
#include <poll.h>
#include <unistd.h>
#include <utility>

namespace fidget {
namespace {

constexpr int ReaderPollIntervalMilliseconds = 100;

std::once_flag ignoreSigpipeOnce;

void IgnoreSigpipe()
{
    std::call_once(ignoreSigpipeOnce, [] {
        // A dead bridge child must become a transport error, not terminate
        // FIDGET while it writes to the child's stdin pipe.
        std::signal(SIGPIPE, SIG_IGN);
    });
}

std::string DescriptorError(const char* operation)
{
    return std::string(operation) + ": " + std::strerror(errno);
}

} // namespace

BridgeConnection::BridgeConnection(
    const int readDescriptor,
    const int writeDescriptor,
    const std::chrono::milliseconds handshakeTimeout,
    const std::size_t queueCapacity)
    : readDescriptor_(readDescriptor)
    , writeDescriptor_(writeDescriptor)
    , handshakeTimeout_(handshakeTimeout)
    , queueCapacity_(queueCapacity)
{
    IgnoreSigpipe();
}

BridgeConnection::~BridgeConnection()
{
    Close();
}

TransportOperationResult BridgeConnection::OpenChannel(
    const BridgeChannel channel,
    const std::string& host,
    const std::uint16_t port)
{
    std::lock_guard<std::mutex> openLock(openMutex_);

    bool startReader = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (channel != BridgeChannel::Command
            && channel != BridgeChannel::Data)
        {
            return {false, "A bridge transport cannot open the hello channel"};
        }
        if (closed_)
        {
            return {false, "Bridge connection is closed"};
        }
        if (!failure_.empty())
        {
            return {false, failure_};
        }
        if (readDescriptor_ < 0 || writeDescriptor_ < 0)
        {
            return {false, "Bridge connection has invalid descriptors"};
        }
        if (handshakeTimeout_.count() <= 0)
        {
            return {false, "Bridge hello timeout must be positive"};
        }
        if (queueCapacity_ == 0U)
        {
            return {false, "Bridge queue capacity must be positive"};
        }

        std::string endpointError;
        if (!ValidateEndpointLocked(channel, host, port, endpointError))
        {
            return {false, std::move(endpointError)};
        }
        if (ChannelOpenLocked(channel))
        {
            return {true, {}};
        }

        SetChannelOpenLocked(channel, true);
        if (!readerStarted_)
        {
            readerStarted_ = true;
            startReader = true;
        }
    }

    if (!startReader)
    {
        return {true, {}};
    }

    try
    {
        readerThread_ = std::thread(&BridgeConnection::ReadLoop, this);
    }
    catch (const std::exception& exception)
    {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            FailLocked(
                std::string("Could not start bridge reader: ")
                + exception.what());
        }
        StopAndJoin();
        return {false, failure_};
    }

    const auto hello = EncodeBridgeHelloFrame();
    if (!hello.success)
    {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            FailLocked(hello.error);
        }
        StopAndJoin();
        return {false, hello.error};
    }

    const auto writeResult = WriteFrame(hello.bytes);
    if (!writeResult.success)
    {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            FailLocked(writeResult.error);
        }
        StopAndJoin();
        return writeResult;
    }

    std::string openError;
    {
        std::unique_lock<std::mutex> lock(stateMutex_);
        const bool finished = stateChanged_.wait_for(
            lock,
            handshakeTimeout_,
            [this] {
                return handshakeComplete_ || !failure_.empty()
                    || stopRequested_;
            });
        if (!finished)
        {
            FailLocked("Bridge hello timed out");
        }
        if (!handshakeComplete_)
        {
            openError = failure_.empty()
                ? "Bridge connection closed during hello"
                : failure_;
        }
    }

    if (!openError.empty())
    {
        StopAndJoin();
        return {false, std::move(openError)};
    }
    return {true, {}};
}

TransportOperationResult BridgeConnection::Send(
    const BridgeChannel channel,
    const std::byte* data,
    const std::size_t size)
{
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (channel != BridgeChannel::Command
            && channel != BridgeChannel::Data)
        {
            return {false, "A bridge transport cannot send a hello frame"};
        }
        if (!failure_.empty())
        {
            return {false, failure_};
        }
        if (!ChannelOpenLocked(channel))
        {
            return {false, ClosedError(channel)};
        }
        if (!handshakeComplete_)
        {
            return {false, "Bridge hello is incomplete"};
        }
    }

    const auto encoded = EncodeBridgeFrame(channel, data, size);
    if (!encoded.success)
    {
        return {false, encoded.error};
    }

    const auto result = WriteFrame(encoded.bytes);
    if (!result.success)
    {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            FailLocked(result.error);
        }
        StopAndJoin();
    }
    return result;
}

TransportReceiveResult BridgeConnection::Receive(
    const BridgeChannel channel,
    std::byte* buffer,
    const std::size_t capacity,
    const int timeoutMilliseconds)
{
    if (channel != BridgeChannel::Command
        && channel != BridgeChannel::Data)
    {
        return {
            TransportReceiveStatus::Error,
            0U,
            "A bridge transport cannot receive the hello channel",
        };
    }
    if (timeoutMilliseconds < 0)
    {
        return {
            TransportReceiveStatus::Error,
            0U,
            InvalidTimeoutError(channel),
        };
    }
    if (capacity > 0U && buffer == nullptr)
    {
        return {
            TransportReceiveStatus::Error,
            0U,
            channel == BridgeChannel::Command
                ? "receive: buffer pointer is null"
                : "data receive: buffer pointer is null",
        };
    }

    std::unique_lock<std::mutex> lock(stateMutex_);
    DatagramQueue& queue = QueueLocked(channel);
    const bool ready = stateChanged_.wait_for(
        lock,
        std::chrono::milliseconds(timeoutMilliseconds),
        [this, channel, &queue] {
            return !queue.empty() || !failure_.empty()
                || !ChannelOpenLocked(channel);
        });

    if (!queue.empty())
    {
        Datagram datagram = std::move(queue.front());
        queue.pop_front();
        const std::size_t copied = std::min(capacity, datagram.size());
        if (copied > 0U)
        {
            std::copy_n(datagram.data(), copied, buffer);
        }
        return {TransportReceiveStatus::Received, copied, {}};
    }
    if (!failure_.empty())
    {
        return {TransportReceiveStatus::Error, 0U, failure_};
    }
    if (!ChannelOpenLocked(channel))
    {
        return {
            TransportReceiveStatus::Error,
            0U,
            ReceiveClosedError(channel),
        };
    }
    if (!ready)
    {
        return {
            TransportReceiveStatus::Timeout,
            0U,
            TimeoutError(channel),
        };
    }

    return {
        TransportReceiveStatus::Error,
        0U,
        "Bridge receive reached an invalid state",
    };
}

void BridgeConnection::CloseChannel(const BridgeChannel channel) noexcept
{
    bool closeConnection = false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        if (channel != BridgeChannel::Command
            && channel != BridgeChannel::Data)
        {
            return;
        }
        SetChannelOpenLocked(channel, false);
        QueueLocked(channel).clear();
        closeConnection = !commandOpen_ && !dataOpen_;
        stateChanged_.notify_all();
    }
    if (closeConnection)
    {
        Close();
    }
}

void BridgeConnection::Close() noexcept
{
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        commandOpen_ = false;
        dataOpen_ = false;
        commandQueue_.clear();
        dataQueue_.clear();
        stopRequested_ = true;
        stateChanged_.notify_all();
    }
    StopAndJoin();
}

bool BridgeConnection::ValidateEndpointLocked(
    const BridgeChannel channel,
    const std::string& host,
    const std::uint16_t port,
    std::string& error)
{
    if (channel == BridgeChannel::Command)
    {
        if (!endpointSet_)
        {
            endpointHost_ = host;
            commandPort_ = port;
            endpointSet_ = true;
            return true;
        }
        if (endpointHost_ != host || commandPort_ != port)
        {
            error = "Bridge command endpoint does not match its relay";
            return false;
        }
        return true;
    }

    if (!endpointSet_)
    {
        error = "Open the bridge command transport before its data receiver";
        return false;
    }
    if (commandPort_ == 0xFFFFU)
    {
        error = "MVLC command port has no adjacent data port";
        return false;
    }
    if (endpointHost_ != host
        || port != static_cast<std::uint16_t>(commandPort_ + 1U))
    {
        error = "Bridge data endpoint does not match its relay";
        return false;
    }
    return true;
}

bool BridgeConnection::ChannelOpenLocked(const BridgeChannel channel) const
{
    return channel == BridgeChannel::Command ? commandOpen_ : dataOpen_;
}

void BridgeConnection::SetChannelOpenLocked(
    const BridgeChannel channel, const bool open)
{
    if (channel == BridgeChannel::Command)
    {
        commandOpen_ = open;
    }
    else
    {
        dataOpen_ = open;
    }
}

BridgeConnection::DatagramQueue& BridgeConnection::QueueLocked(
    const BridgeChannel channel)
{
    return channel == BridgeChannel::Command ? commandQueue_ : dataQueue_;
}

const char* BridgeConnection::ClosedError(
    const BridgeChannel channel) const
{
    return channel == BridgeChannel::Command
        ? "send: bridge command transport is closed"
        : "data send: bridge data receiver is closed";
}

const char* BridgeConnection::ReceiveClosedError(
    const BridgeChannel channel) const
{
    return channel == BridgeChannel::Command
        ? "receive: bridge command transport is closed"
        : "data receive: bridge data receiver is closed";
}

const char* BridgeConnection::InvalidTimeoutError(
    const BridgeChannel channel) const
{
    return channel == BridgeChannel::Command
        ? "receive: invalid timeout"
        : "data receive: invalid timeout";
}

const char* BridgeConnection::TimeoutError(
    const BridgeChannel channel) const
{
    return channel == BridgeChannel::Command
        ? "receive: MVLC response timed out"
        : "data receive: timed out";
}

TransportOperationResult BridgeConnection::WriteFrame(
    const std::vector<std::byte>& frameBytes)
{
    std::lock_guard<std::mutex> lock(writeMutex_);
    if (writeDescriptor_ < 0)
    {
        return {false, "bridge write: connection is closed"};
    }

    std::size_t offset = 0U;
    while (offset < frameBytes.size())
    {
        const ssize_t written = ::write(
            writeDescriptor_,
            frameBytes.data() + offset,
            frameBytes.size() - offset);
        if (written < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            return {false, DescriptorError("bridge write")};
        }
        if (written == 0)
        {
            return {false, "bridge write: wrote zero bytes"};
        }
        offset += static_cast<std::size_t>(written);
    }
    return {true, {}};
}

void BridgeConnection::ReadLoop()
{
    BridgeFrameDecoder decoder;
    std::array<std::byte, 8192U> buffer{};

    while (true)
    {
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (stopRequested_)
            {
                return;
            }
        }

        pollfd descriptor{};
        descriptor.fd = readDescriptor_;
        descriptor.events = POLLIN;

        int pollResult = -1;
        do
        {
            pollResult = ::poll(
                &descriptor, 1U, ReaderPollIntervalMilliseconds);
        } while (pollResult < 0 && errno == EINTR);

        if (pollResult < 0)
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            FailLocked(DescriptorError("bridge poll"));
            return;
        }
        if (pollResult == 0)
        {
            continue;
        }
        if ((descriptor.revents & POLLNVAL) != 0)
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            FailLocked("bridge poll: invalid descriptor");
            return;
        }
        if ((descriptor.revents & (POLLIN | POLLHUP | POLLERR)) == 0)
        {
            continue;
        }

        ssize_t received = -1;
        do
        {
            received = ::read(
                readDescriptor_, buffer.data(), buffer.size());
        } while (received < 0 && errno == EINTR);

        if (received < 0)
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            FailLocked(DescriptorError("bridge read"));
            return;
        }
        if (received == 0)
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (!stopRequested_)
            {
                FailLocked("Bridge peer closed the stream");
            }
            return;
        }

        auto decoded = decoder.Consume(
            buffer.data(), static_cast<std::size_t>(received));
        if (!decoded.success)
        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            FailLocked(std::move(decoded.error));
            return;
        }
        if (!RouteFrames(std::move(decoded.frames)))
        {
            return;
        }
    }
}

bool BridgeConnection::RouteFrames(std::vector<BridgeFrame> frames)
{
    std::lock_guard<std::mutex> lock(stateMutex_);
    for (auto& frame : frames)
    {
        if (frame.channel == BridgeChannel::Hello)
        {
            handshakeComplete_ = true;
            stateChanged_.notify_all();
            continue;
        }

        if (!ChannelOpenLocked(frame.channel))
        {
            continue;
        }

        DatagramQueue& queue = QueueLocked(frame.channel);
        if (queue.size() >= queueCapacity_)
        {
            FailLocked(
                frame.channel == BridgeChannel::Command
                    ? "Bridge command queue is full"
                    : "Bridge data queue is full");
            return false;
        }
        queue.push_back(std::move(frame.payload));
    }
    stateChanged_.notify_all();
    return true;
}

void BridgeConnection::FailLocked(std::string error)
{
    if (failure_.empty())
    {
        failure_ = std::move(error);
    }
    commandOpen_ = false;
    dataOpen_ = false;
    stopRequested_ = true;
    stateChanged_.notify_all();
}

void BridgeConnection::StopAndJoin() noexcept
{
    std::lock_guard<std::mutex> shutdownLock(shutdownMutex_);
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        stopRequested_ = true;
        stateChanged_.notify_all();
    }

    CloseWriteDescriptor();
    if (readerThread_.joinable()
        && readerThread_.get_id() != std::this_thread::get_id())
    {
        readerThread_.join();
    }
    CloseReadDescriptor();

    std::lock_guard<std::mutex> lock(stateMutex_);
    closed_ = true;
}

void BridgeConnection::CloseReadDescriptor() noexcept
{
    if (readDescriptor_ >= 0)
    {
        ::close(readDescriptor_);
        readDescriptor_ = -1;
    }
}

void BridgeConnection::CloseWriteDescriptor() noexcept
{
    std::lock_guard<std::mutex> lock(writeMutex_);
    if (writeDescriptor_ >= 0)
    {
        ::close(writeDescriptor_);
        writeDescriptor_ = -1;
    }
}

} // namespace fidget
