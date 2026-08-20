#include "hardware/BridgeRelay.h"

#include "core/BridgeProtocol.h"

#include <arpa/inet.h>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <netdb.h>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace fidget {
namespace {

constexpr std::size_t StreamReadBufferSize = 8192U;

std::string SystemError(const char* operation)
{
    return std::string(operation) + ": " + std::strerror(errno);
}

class UdpEndpoint
{
public:
    UdpEndpoint() = default;

    ~UdpEndpoint()
    {
        Close();
    }

    UdpEndpoint(const UdpEndpoint&) = delete;
    UdpEndpoint& operator=(const UdpEndpoint&) = delete;

    bool Open(
        const std::string& host,
        const std::uint16_t port,
        const char* label,
        std::string& error)
    {
        Close();

        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        hints.ai_protocol = IPPROTO_UDP;

        addrinfo* addresses = nullptr;
        const std::string service = std::to_string(port);
        const int lookupResult = ::getaddrinfo(
            host.c_str(), service.c_str(), &hints, &addresses);
        if (lookupResult != 0)
        {
            error = std::string(label) + " address lookup failed: "
                + ::gai_strerror(lookupResult);
            return false;
        }

        error = std::string("No usable IPv4 ") + label + " address";
        for (addrinfo* address = addresses;
             address != nullptr;
             address = address->ai_next)
        {
            const int candidate = ::socket(
                address->ai_family,
                address->ai_socktype,
                address->ai_protocol);
            if (candidate < 0)
            {
                error = SystemError("socket");
                continue;
            }

            sockaddr_in localAddress{};
            localAddress.sin_family = AF_INET;
            localAddress.sin_addr.s_addr = htonl(INADDR_ANY);
            localAddress.sin_port = 0U;
            if (::bind(
                    candidate,
                    reinterpret_cast<const sockaddr*>(&localAddress),
                    sizeof(localAddress)) != 0)
            {
                error = SystemError("bind");
                ::close(candidate);
                continue;
            }

            descriptor_ = candidate;
            destinationLength_ = static_cast<socklen_t>(address->ai_addrlen);
            std::memcpy(
                &destination_, address->ai_addr, address->ai_addrlen);
            break;
        }
        ::freeaddrinfo(addresses);
        return descriptor_ >= 0;
    }

    bool Send(
        const std::byte* data,
        const std::size_t size,
        std::string& error) const
    {
        const std::byte emptyPayload{};
        const std::byte* sendData = size == 0U ? &emptyPayload : data;
        ssize_t sent = -1;
        do
        {
            sent = ::sendto(
                descriptor_,
                sendData,
                size,
                0,
                reinterpret_cast<const sockaddr*>(&destination_),
                destinationLength_);
        } while (sent < 0 && errno == EINTR);

        if (sent < 0)
        {
            error = SystemError("sendto");
            return false;
        }
        if (static_cast<std::size_t>(sent) != size)
        {
            error = "sendto: short UDP datagram";
            return false;
        }
        return true;
    }

    bool Receive(
        std::byte* buffer,
        const std::size_t capacity,
        std::size_t& receivedSize,
        bool& accepted,
        std::string& error) const
    {
        sockaddr_storage sender{};
        socklen_t senderLength = sizeof(sender);
        ssize_t received = -1;
        do
        {
            received = ::recvfrom(
                descriptor_,
                buffer,
                capacity,
                0,
                reinterpret_cast<sockaddr*>(&sender),
                &senderLength);
        } while (received < 0 && errno == EINTR);

        if (received < 0)
        {
            error = SystemError("recvfrom");
            return false;
        }

        receivedSize = static_cast<std::size_t>(received);
        accepted = IsExpectedSender(sender, senderLength);
        return true;
    }

    [[nodiscard]] int Descriptor() const noexcept
    {
        return descriptor_;
    }

private:
    bool IsExpectedSender(
        const sockaddr_storage& sender,
        const socklen_t senderLength) const noexcept
    {
        if (senderLength < sizeof(sockaddr_in)
            || sender.ss_family != AF_INET
            || destination_.ss_family != AF_INET)
        {
            return false;
        }

        const auto* senderIpv4 = reinterpret_cast<const sockaddr_in*>(&sender);
        const auto* destinationIpv4 =
            reinterpret_cast<const sockaddr_in*>(&destination_);
        return senderIpv4->sin_port == destinationIpv4->sin_port
            && senderIpv4->sin_addr.s_addr
                == destinationIpv4->sin_addr.s_addr;
    }

    void Close() noexcept
    {
        if (descriptor_ >= 0)
        {
            ::close(descriptor_);
            descriptor_ = -1;
        }
        destination_ = {};
        destinationLength_ = 0U;
    }

    int descriptor_ = -1;
    sockaddr_storage destination_{};
    socklen_t destinationLength_ = 0U;
};

bool WriteAll(
    const int descriptor,
    const std::byte* data,
    const std::size_t size,
    std::string& error)
{
    std::size_t written = 0U;
    while (written < size)
    {
        ssize_t result = -1;
        do
        {
            result = ::write(
                descriptor, data + written, size - written);
        } while (result < 0 && errno == EINTR);

        if (result < 0)
        {
            error = SystemError("write");
            return false;
        }
        if (result == 0)
        {
            error = "write: no progress";
            return false;
        }
        written += static_cast<std::size_t>(result);
    }
    return true;
}

bool SendInputFrames(
    const BridgeFrameDecodeResult& decoded,
    const UdpEndpoint& commandEndpoint,
    const UdpEndpoint& dataEndpoint,
    std::string& error)
{
    for (const BridgeFrame& frame : decoded.frames)
    {
        if (frame.channel == BridgeChannel::Hello)
        {
            continue;
        }

        const UdpEndpoint& endpoint =
            frame.channel == BridgeChannel::Command
            ? commandEndpoint
            : dataEndpoint;
        if (!endpoint.Send(
                frame.payload.data(), frame.payload.size(), error))
        {
            return false;
        }
    }
    return true;
}

bool ReceiveAndWriteFrame(
    const UdpEndpoint& endpoint,
    const BridgeChannel channel,
    const int outputDescriptor,
    std::vector<std::byte>& datagramBuffer,
    std::string& error)
{
    std::size_t receivedSize = 0U;
    bool accepted = false;
    if (!endpoint.Receive(
            datagramBuffer.data(),
            datagramBuffer.size(),
            receivedSize,
            accepted,
            error))
    {
        return false;
    }
    if (!accepted)
    {
        return true;
    }

    const auto encoded = EncodeBridgeFrame(
        channel, datagramBuffer.data(), receivedSize);
    if (!encoded.success)
    {
        error = encoded.error;
        return false;
    }
    return WriteAll(
        outputDescriptor, encoded.bytes.data(), encoded.bytes.size(), error);
}

BridgeRelayResult Failure(std::string error)
{
    return {false, std::move(error)};
}

} // namespace

BridgeRelayResult RunBridgeRelay(
    const std::string& mvlcHost,
    const std::uint16_t commandPort,
    const int inputDescriptor,
    const int outputDescriptor)
{
    if (mvlcHost.empty())
    {
        return Failure("The MVLC host is empty");
    }
    if (commandPort == 0U)
    {
        return Failure("The MVLC command port is zero");
    }
    if (commandPort == 0xFFFFU)
    {
        return Failure("The MVLC command port has no adjacent data port.");
    }
    if (inputDescriptor < 0 || outputDescriptor < 0)
    {
        return Failure("The bridge stream descriptor is invalid");
    }

    const auto hello = EncodeBridgeHelloFrame();
    if (!hello.success)
    {
        return Failure(hello.error);
    }
    std::string error;
    if (!WriteAll(
            outputDescriptor,
            hello.bytes.data(),
            hello.bytes.size(),
            error))
    {
        return Failure(std::move(error));
    }

    UdpEndpoint commandEndpoint;
    if (!commandEndpoint.Open(
            mvlcHost, commandPort, "command", error))
    {
        return Failure(std::move(error));
    }
    UdpEndpoint dataEndpoint;
    const auto dataPort = static_cast<std::uint16_t>(commandPort + 1U);
    if (!dataEndpoint.Open(mvlcHost, dataPort, "data", error))
    {
        return Failure(std::move(error));
    }

    BridgeFrameDecoder decoder;
    std::array<std::byte, StreamReadBufferSize> streamBuffer{};
    std::vector<std::byte> datagramBuffer(BridgeMaximumPayloadSize);
    std::array<pollfd, 3> pollDescriptors{{
        {inputDescriptor, POLLIN | POLLHUP, 0},
        {commandEndpoint.Descriptor(), POLLIN, 0},
        {dataEndpoint.Descriptor(), POLLIN, 0},
    }};

    while (true)
    {
        int pollResult = -1;
        do
        {
            pollResult = ::poll(
                pollDescriptors.data(), pollDescriptors.size(), -1);
        } while (pollResult < 0 && errno == EINTR);
        if (pollResult < 0)
        {
            return Failure(SystemError("poll"));
        }

        pollfd& input = pollDescriptors[0];
        if ((input.revents & (POLLIN | POLLHUP)) != 0)
        {
            ssize_t bytesRead = -1;
            do
            {
                bytesRead = ::read(
                    inputDescriptor,
                    streamBuffer.data(),
                    streamBuffer.size());
            } while (bytesRead < 0 && errno == EINTR);
            if (bytesRead < 0)
            {
                return Failure(SystemError("read"));
            }
            if (bytesRead == 0)
            {
                return {true, {}};
            }

            const auto decoded = decoder.Consume(
                streamBuffer.data(), static_cast<std::size_t>(bytesRead));
            if (!decoded.success)
            {
                return Failure("bridge input: " + decoded.error);
            }
            if (!SendInputFrames(
                    decoded, commandEndpoint, dataEndpoint, error))
            {
                return Failure(std::move(error));
            }
        }
        if ((input.revents & (POLLERR | POLLNVAL)) != 0)
        {
            return Failure("poll: bridge input failed");
        }

        pollfd& command = pollDescriptors[1];
        if ((command.revents & POLLIN) != 0
            && !ReceiveAndWriteFrame(
                commandEndpoint,
                BridgeChannel::Command,
                outputDescriptor,
                datagramBuffer,
                error))
        {
            return Failure(std::move(error));
        }
        if ((command.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            return Failure("poll: command socket failed");
        }

        pollfd& data = pollDescriptors[2];
        if ((data.revents & POLLIN) != 0
            && !ReceiveAndWriteFrame(
                dataEndpoint,
                BridgeChannel::Data,
                outputDescriptor,
                datagramBuffer,
                error))
        {
            return Failure(std::move(error));
        }
        if ((data.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            return Failure("poll: data socket failed");
        }
    }
}

} // namespace fidget
