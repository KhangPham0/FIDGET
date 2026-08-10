#include "hardware/MvlcDataReceiver.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netdb.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <utility>

namespace fidget {
namespace {

std::string DataSocketError(const char* operation)
{
    return std::string(operation) + ": " + std::strerror(errno);
}

bool SetDataReceiveTimeout(int socketDescriptor,
                           int timeoutMilliseconds,
                           std::string& error)
{
    if (timeoutMilliseconds < 0)
    {
        error = "data receive: invalid timeout";
        return false;
    }

    const timeval timeout{
        timeoutMilliseconds / 1000,
        (timeoutMilliseconds % 1000) * 1000,
    };
    if (::setsockopt(
            socketDescriptor,
            SOL_SOCKET,
            SO_RCVTIMEO,
            &timeout,
            sizeof(timeout)) != 0)
    {
        error = DataSocketError("data setsockopt");
        return false;
    }
    return true;
}

} // namespace

MvlcDataReceiver::~MvlcDataReceiver()
{
    Close();
}

TransportOperationResult MvlcDataReceiver::Open(
    const std::string& host,
    std::uint16_t port)
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
        return {
            false,
            std::string("Data address lookup failed: ")
                + ::gai_strerror(lookupResult),
        };
    }

    std::string lastError = "No usable IPv4 data address";
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
            lastError = DataSocketError("data socket");
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
            lastError = DataSocketError("data bind");
            ::close(candidate);
            continue;
        }

        if (::connect(candidate, address->ai_addr, address->ai_addrlen) == 0)
        {
            socketDescriptor_ = candidate;
            break;
        }

        lastError = DataSocketError("data connect");
        ::close(candidate);
    }
    ::freeaddrinfo(addresses);

    if (socketDescriptor_ < 0)
    {
        return {false, lastError};
    }
    return {true, {}};
}

TransportOperationResult MvlcDataReceiver::Send(
    const std::byte* data,
    std::size_t size)
{
    if (socketDescriptor_ < 0)
    {
        return {false, "data send: MVLC data receiver is closed"};
    }

    ssize_t sent = -1;
    do
    {
        sent = ::send(socketDescriptor_, data, size, 0);
    } while (sent < 0 && errno == EINTR);

    if (sent < 0)
    {
        return {false, DataSocketError("data send")};
    }
    if (static_cast<std::size_t>(sent) != size)
    {
        return {false, "data send: short UDP datagram"};
    }
    return {true, {}};
}

TransportReceiveResult MvlcDataReceiver::Receive(
    std::byte* buffer,
    std::size_t capacity,
    int timeoutMilliseconds)
{
    if (socketDescriptor_ < 0)
    {
        return {
            TransportReceiveStatus::Error,
            0U,
            "data receive: MVLC data receiver is closed",
        };
    }

    std::string timeoutError;
    if (!SetDataReceiveTimeout(
            socketDescriptor_, timeoutMilliseconds, timeoutError))
    {
        return {
            TransportReceiveStatus::Error,
            0U,
            std::move(timeoutError),
        };
    }

    ssize_t received = -1;
    do
    {
        received = ::recv(socketDescriptor_, buffer, capacity, 0);
    } while (received < 0 && errno == EINTR);

    if (received < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return {
                TransportReceiveStatus::Timeout,
                0U,
                "data receive: timed out",
            };
        }
        return {
            TransportReceiveStatus::Error,
            0U,
            DataSocketError("data receive"),
        };
    }

    return {
        TransportReceiveStatus::Received,
        static_cast<std::size_t>(received),
        {},
    };
}

void MvlcDataReceiver::Close() noexcept
{
    if (socketDescriptor_ < 0)
    {
        return;
    }

    ::shutdown(socketDescriptor_, SHUT_RDWR);
    ::close(socketDescriptor_);
    socketDescriptor_ = -1;
}

bool MvlcDataReceiver::IsOpen() const noexcept
{
    return socketDescriptor_ >= 0;
}

} // namespace fidget
