#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "hardware/MvlcCommandTransport.h"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

class LocalUdpServer
{
public:
    LocalUdpServer()
    {
        descriptor_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        REQUIRE(descriptor_ >= 0);

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0U;
        REQUIRE(::bind(
            descriptor_,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) == 0);

        socklen_t addressLength = sizeof(address);
        REQUIRE(::getsockname(
            descriptor_,
            reinterpret_cast<sockaddr*>(&address),
            &addressLength) == 0);
        port_ = ntohs(address.sin_port);

        const timeval timeout{1, 0};
        REQUIRE(::setsockopt(
            descriptor_,
            SOL_SOCKET,
            SO_RCVTIMEO,
            &timeout,
            sizeof(timeout)) == 0);
    }

    ~LocalUdpServer()
    {
        if (descriptor_ >= 0)
        {
            ::close(descriptor_);
        }
    }

    LocalUdpServer(const LocalUdpServer&) = delete;
    LocalUdpServer& operator=(const LocalUdpServer&) = delete;

    [[nodiscard]] std::uint16_t Port() const noexcept
    {
        return port_;
    }

    std::size_t Receive(std::byte* buffer,
                        std::size_t capacity,
                        sockaddr_in& sender)
    {
        socklen_t senderLength = sizeof(sender);
        const ssize_t received = ::recvfrom(
            descriptor_,
            buffer,
            capacity,
            0,
            reinterpret_cast<sockaddr*>(&sender),
            &senderLength);
        REQUIRE(received >= 0);
        return static_cast<std::size_t>(received);
    }

    void Send(const std::byte* data,
              std::size_t size,
              const sockaddr_in& destination)
    {
        const ssize_t sent = ::sendto(
            descriptor_,
            data,
            size,
            0,
            reinterpret_cast<const sockaddr*>(&destination),
            sizeof(destination));
        REQUIRE(sent == static_cast<ssize_t>(size));
    }

private:
    int descriptor_ = -1;
    std::uint16_t port_ = 0U;
};

} // namespace

TEST_CASE("the MVLC command transport sends receives times out and closes")
{
    using namespace fidget;

    LocalUdpServer server;
    MvlcCommandTransport transport;
    const auto opened = transport.Open("127.0.0.1", server.Port());
    INFO(opened.error);
    REQUIRE(opened.success);
    CHECK(transport.IsOpen());

    constexpr std::array<std::byte, 5> request{
        std::byte{0x10U},
        std::byte{0x20U},
        std::byte{0x30U},
        std::byte{0x40U},
        std::byte{0x50U},
    };
    REQUIRE(transport.Send(request.data(), request.size()).success);

    std::array<std::byte, 32> serverBuffer{};
    sockaddr_in sender{};
    const auto requestSize = server.Receive(
        serverBuffer.data(), serverBuffer.size(), sender);
    REQUIRE(requestSize == request.size());
    CHECK(std::equal(
        request.begin(), request.end(), serverBuffer.begin()));

    constexpr std::array<std::byte, 4> reply{
        std::byte{0xAAU},
        std::byte{0xBBU},
        std::byte{0xCCU},
        std::byte{0xDDU},
    };
    server.Send(reply.data(), reply.size(), sender);

    std::array<std::byte, 32> clientBuffer{};
    const auto received = transport.Receive(
        clientBuffer.data(), clientBuffer.size(), 1000);
    INFO(received.error);
    REQUIRE(received.status == TransportReceiveStatus::Received);
    REQUIRE(received.bytesReceived == reply.size());
    CHECK(std::equal(reply.begin(), reply.end(), clientBuffer.begin()));

    const auto timeout = transport.Receive(
        clientBuffer.data(), clientBuffer.size(), 25);
    CHECK(timeout.status == TransportReceiveStatus::Timeout);
    CHECK(timeout.bytesReceived == 0U);
    CHECK(timeout.error == "receive: MVLC response timed out");

    transport.Close();
    CHECK_FALSE(transport.IsOpen());
    CHECK_FALSE(transport.Send(request.data(), request.size()).success);
}
