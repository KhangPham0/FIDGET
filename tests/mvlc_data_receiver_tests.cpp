#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "hardware/MvlcDataReceiver.h"

#include <algorithm>
#include <arpa/inet.h>
#include <array>
#include <cstddef>
#include <cstdint>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

namespace {

class LocalDataSender
{
public:
    LocalDataSender()
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

    ~LocalDataSender()
    {
        if (descriptor_ >= 0)
        {
            ::close(descriptor_);
        }
    }

    LocalDataSender(const LocalDataSender&) = delete;
    LocalDataSender& operator=(const LocalDataSender&) = delete;

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

TEST_CASE("the MVLC data receiver binds receives times out and closes")
{
    using namespace fidget;

    LocalDataSender sender;
    MvlcDataReceiver receiver;
    const auto opened = receiver.Open("127.0.0.1", sender.Port());
    INFO(opened.error);
    REQUIRE(opened.success);
    CHECK(receiver.IsOpen());

    constexpr std::array<std::byte, 2> redirect{
        std::byte{0xF1U}, std::byte{0xF2U}};
    REQUIRE(receiver.Send(redirect.data(), redirect.size()).success);

    std::array<std::byte, 32> senderBuffer{};
    sockaddr_in receiverAddress{};
    const auto redirectSize = sender.Receive(
        senderBuffer.data(), senderBuffer.size(), receiverAddress);
    REQUIRE(redirectSize == redirect.size());
    CHECK(std::equal(
        redirect.begin(), redirect.end(), senderBuffer.begin()));

    constexpr std::array<std::byte, 6> data{
        std::byte{0x01U},
        std::byte{0x02U},
        std::byte{0x03U},
        std::byte{0x04U},
        std::byte{0x05U},
        std::byte{0x06U},
    };
    sender.Send(data.data(), data.size(), receiverAddress);

    std::array<std::byte, 32> receiveBuffer{};
    const auto received = receiver.Receive(
        receiveBuffer.data(), receiveBuffer.size(), 1000);
    INFO(received.error);
    REQUIRE(received.status == TransportReceiveStatus::Received);
    REQUIRE(received.bytesReceived == data.size());
    CHECK(std::equal(data.begin(), data.end(), receiveBuffer.begin()));

    const auto timeout = receiver.Receive(
        receiveBuffer.data(), receiveBuffer.size(), 25);
    CHECK(timeout.status == TransportReceiveStatus::Timeout);
    CHECK(timeout.error == "data receive: timed out");

    receiver.Close();
    CHECK_FALSE(receiver.IsOpen());
    CHECK_FALSE(receiver.Send(redirect.data(), redirect.size()).success);
}
