#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/BridgeProtocol.h"
#include "hardware/BridgeCommandTransport.h"
#include "hardware/BridgeConnection.h"
#include "hardware/BridgeDataReceiver.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <future>
#include <initializer_list>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

constexpr std::uint16_t CommandPort = 32768U;

std::vector<std::byte> Bytes(
    const std::initializer_list<std::uint8_t> values)
{
    std::vector<std::byte> bytes;
    bytes.reserve(values.size());
    for (const std::uint8_t value : values)
    {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

bool WriteAll(const int descriptor, const std::vector<std::byte>& bytes)
{
    std::size_t offset = 0U;
    while (offset < bytes.size())
    {
        const ssize_t written = ::write(
            descriptor, bytes.data() + offset, bytes.size() - offset);
        if (written < 0 && errno == EINTR)
        {
            continue;
        }
        if (written <= 0)
        {
            return false;
        }
        offset += static_cast<std::size_t>(written);
    }
    return true;
}

struct PeerReceiveResult
{
    bool success = false;
    fidget::BridgeFrame frame;
    std::string error;
};

class ScriptedBridgePeer
{
public:
    explicit ScriptedBridgePeer(const int descriptor)
        : descriptor_(descriptor)
    {
    }

    ~ScriptedBridgePeer()
    {
        Close();
    }

    ScriptedBridgePeer(const ScriptedBridgePeer&) = delete;
    ScriptedBridgePeer& operator=(const ScriptedBridgePeer&) = delete;

    PeerReceiveResult ReceiveFrame()
    {
        while (frames_.empty())
        {
            pollfd descriptor{};
            descriptor.fd = descriptor_;
            descriptor.events = POLLIN;

            int pollResult = -1;
            do
            {
                pollResult = ::poll(&descriptor, 1U, 1000);
            } while (pollResult < 0 && errno == EINTR);
            if (pollResult <= 0)
            {
                return {false, {}, "Timed out reading a bridge frame"};
            }

            std::array<std::byte, 4096U> buffer{};
            ssize_t received = -1;
            do
            {
                received = ::read(
                    descriptor_, buffer.data(), buffer.size());
            } while (received < 0 && errno == EINTR);
            if (received <= 0)
            {
                return {false, {}, "Bridge peer reached EOF"};
            }

            auto decoded = decoder_.Consume(
                buffer.data(), static_cast<std::size_t>(received));
            if (!decoded.success)
            {
                return {false, {}, decoded.error};
            }
            for (auto& frame : decoded.frames)
            {
                frames_.push_back(std::move(frame));
            }
        }

        auto frame = std::move(frames_.front());
        frames_.pop_front();
        return {true, std::move(frame), {}};
    }

    bool SendFrame(
        const fidget::BridgeChannel channel,
        const std::vector<std::byte>& payload)
    {
        const auto encoded = fidget::EncodeBridgeFrame(
            channel, payload.data(), payload.size());
        return encoded.success && WriteAll(descriptor_, encoded.bytes);
    }

    bool SendHello()
    {
        const auto encoded = fidget::EncodeBridgeHelloFrame();
        return encoded.success && WriteAll(descriptor_, encoded.bytes);
    }

    bool SendVersionTwoHello()
    {
        const std::vector<std::byte> bytes{
            std::byte{0x7FU},
            std::byte{0x00U},
            std::byte{0x00U},
            std::byte{0x00U},
            std::byte{0x01U},
            std::byte{0x02U},
        };
        return WriteAll(descriptor_, bytes);
    }

    bool WaitForEof()
    {
        pollfd descriptor{};
        descriptor.fd = descriptor_;
        descriptor.events = POLLIN;

        int pollResult = -1;
        do
        {
            pollResult = ::poll(&descriptor, 1U, 1000);
        } while (pollResult < 0 && errno == EINTR);
        if (pollResult <= 0)
        {
            return false;
        }

        std::byte byte{};
        ssize_t received = -1;
        do
        {
            received = ::read(descriptor_, &byte, 1U);
        } while (received < 0 && errno == EINTR);
        return received == 0;
    }

    void Close()
    {
        if (descriptor_ >= 0)
        {
            ::close(descriptor_);
            descriptor_ = -1;
        }
    }

private:
    int descriptor_ = -1;
    fidget::BridgeFrameDecoder decoder_;
    std::deque<fidget::BridgeFrame> frames_;
};

struct BridgePair
{
    std::shared_ptr<fidget::BridgeConnection> connection;
    std::unique_ptr<ScriptedBridgePeer> peer;
};

BridgePair MakeBridgePair(
    const std::chrono::milliseconds handshakeTimeout =
        std::chrono::seconds(1))
{
    std::array<int, 2> descriptors{};
    REQUIRE(::socketpair(
        AF_UNIX, SOCK_STREAM, 0, descriptors.data()) == 0);
    const int writeDescriptor = ::dup(descriptors[0]);
    REQUIRE(writeDescriptor >= 0);

    BridgePair pair;
    pair.connection = std::make_shared<fidget::BridgeConnection>(
        descriptors[0], writeDescriptor, handshakeTimeout);
    pair.peer = std::make_unique<ScriptedBridgePeer>(descriptors[1]);
    return pair;
}

fidget::TransportOperationResult CompleteHandshake(
    fidget::BridgeCommandTransport& command,
    ScriptedBridgePeer& peer,
    const bool supportedVersion = true)
{
    auto opened = std::async(std::launch::async, [&command] {
        return command.Open("mvlc-test", CommandPort);
    });

    const auto hello = peer.ReceiveFrame();
    CHECK(hello.success);
    if (hello.success)
    {
        CHECK(hello.frame.channel == fidget::BridgeChannel::Hello);
        CHECK(hello.frame.payload == Bytes({fidget::BridgeProtocolVersion}));
    }
    const bool helloSent = supportedVersion
        ? peer.SendHello()
        : peer.SendVersionTwoHello();
    CHECK(helloSent);
    return opened.get();
}

} // namespace

TEST_CASE("bridge transports route requests and replies by channel")
{
    using namespace fidget;

    auto pair = MakeBridgePair();
    BridgeCommandTransport command(pair.connection);
    BridgeDataReceiver data(pair.connection);
    const auto commandOpened = CompleteHandshake(command, *pair.peer);
    INFO(commandOpened.error);
    REQUIRE(commandOpened.success);

    const auto dataOpened = data.Open(
        "mvlc-test", static_cast<std::uint16_t>(CommandPort + 1U));
    INFO(dataOpened.error);
    REQUIRE(dataOpened.success);

    const auto commandRequest = Bytes({0x10U, 0x20U, 0x30U});
    REQUIRE(command.Send(
        commandRequest.data(), commandRequest.size()).success);
    const auto receivedRequest = pair.peer->ReceiveFrame();
    INFO(receivedRequest.error);
    REQUIRE(receivedRequest.success);
    CHECK(receivedRequest.frame.channel == BridgeChannel::Command);
    CHECK(receivedRequest.frame.payload == commandRequest);

    REQUIRE(data.Send(nullptr, 0U).success);
    const auto redirect = pair.peer->ReceiveFrame();
    INFO(redirect.error);
    REQUIRE(redirect.success);
    CHECK(redirect.frame.channel == BridgeChannel::Data);
    CHECK(redirect.frame.payload.empty());

    const auto dataDatagram = Bytes({0xD1U, 0xD2U, 0xD3U, 0xD4U});
    const auto commandReply = Bytes({0xC1U, 0xC2U});
    REQUIRE(pair.peer->SendFrame(BridgeChannel::Data, dataDatagram));
    REQUIRE(pair.peer->SendFrame(BridgeChannel::Command, commandReply));

    std::array<std::byte, 32U> buffer{};
    const auto commandReceived = command.Receive(
        buffer.data(), buffer.size(), 1000);
    INFO(commandReceived.error);
    REQUIRE(commandReceived.status == TransportReceiveStatus::Received);
    REQUIRE(commandReceived.bytesReceived == commandReply.size());
    CHECK(std::equal(
        commandReply.begin(), commandReply.end(), buffer.begin()));

    const auto dataReceived = data.Receive(
        buffer.data(), buffer.size(), 1000);
    INFO(dataReceived.error);
    REQUIRE(dataReceived.status == TransportReceiveStatus::Received);
    REQUIRE(dataReceived.bytesReceived == dataDatagram.size());
    CHECK(std::equal(
        dataDatagram.begin(), dataDatagram.end(), buffer.begin()));
}

TEST_CASE("bridge transports preserve datagram boundaries")
{
    using namespace fidget;

    auto pair = MakeBridgePair();
    BridgeCommandTransport command(pair.connection);
    BridgeDataReceiver data(pair.connection);
    REQUIRE(CompleteHandshake(command, *pair.peer).success);
    REQUIRE(data.Open(
        "mvlc-test", static_cast<std::uint16_t>(CommandPort + 1U)).success);

    const auto first = Bytes({0x01U, 0x02U});
    const auto second = Bytes({0x03U, 0x04U, 0x05U});
    REQUIRE(pair.peer->SendFrame(BridgeChannel::Data, first));
    REQUIRE(pair.peer->SendFrame(BridgeChannel::Data, second));

    std::array<std::byte, 16U> buffer{};
    const auto firstReceived = data.Receive(
        buffer.data(), buffer.size(), 1000);
    REQUIRE(firstReceived.status == TransportReceiveStatus::Received);
    REQUIRE(firstReceived.bytesReceived == first.size());
    CHECK(std::equal(first.begin(), first.end(), buffer.begin()));

    const auto secondReceived = data.Receive(
        buffer.data(), buffer.size(), 1000);
    REQUIRE(secondReceived.status == TransportReceiveStatus::Received);
    REQUIRE(secondReceived.bytesReceived == second.size());
    CHECK(std::equal(second.begin(), second.end(), buffer.begin()));
}

TEST_CASE("bridge transports retain direct transport timeout semantics")
{
    using namespace fidget;

    auto pair = MakeBridgePair();
    BridgeCommandTransport command(pair.connection);
    BridgeDataReceiver data(pair.connection);
    REQUIRE(CompleteHandshake(command, *pair.peer).success);
    REQUIRE(data.Open(
        "mvlc-test", static_cast<std::uint16_t>(CommandPort + 1U)).success);

    std::array<std::byte, 16U> buffer{};
    const auto commandTimeout = command.Receive(
        buffer.data(), buffer.size(), 25);
    CHECK(commandTimeout.status == TransportReceiveStatus::Timeout);
    CHECK(commandTimeout.bytesReceived == 0U);
    CHECK(commandTimeout.error == "receive: MVLC response timed out");

    const auto dataTimeout = data.Receive(
        buffer.data(), buffer.size(), 25);
    CHECK(dataTimeout.status == TransportReceiveStatus::Timeout);
    CHECK(dataTimeout.bytesReceived == 0U);
    CHECK(dataTimeout.error == "data receive: timed out");

    CHECK(command.Receive(buffer.data(), buffer.size(), -1).error
        == "receive: invalid timeout");
    CHECK(data.Receive(buffer.data(), buffer.size(), -1).error
        == "data receive: invalid timeout");
}

TEST_CASE("bridge connection rejects a mismatched protocol version")
{
    using namespace fidget;

    auto pair = MakeBridgePair();
    BridgeCommandTransport command(pair.connection);
    const auto opened = CompleteHandshake(command, *pair.peer, false);
    CHECK_FALSE(opened.success);
    CHECK(opened.error == "Unsupported bridge protocol version");

    const auto payload = Bytes({0x01U});
    CHECK_FALSE(command.Send(payload.data(), payload.size()).success);
}

TEST_CASE("bridge connection times out when the peer omits its hello")
{
    using namespace fidget;

    auto pair = MakeBridgePair(std::chrono::milliseconds(25));
    BridgeCommandTransport command(pair.connection);
    auto opened = std::async(std::launch::async, [&command] {
        return command.Open("mvlc-test", CommandPort);
    });

    const auto hello = pair.peer->ReceiveFrame();
    INFO(hello.error);
    REQUIRE(hello.success);
    REQUIRE(hello.frame.channel == BridgeChannel::Hello);

    const auto result = opened.get();
    CHECK_FALSE(result.success);
    CHECK(result.error == "Bridge hello timed out");
}

TEST_CASE("closing one bridge channel leaves the other channel usable")
{
    using namespace fidget;

    auto pair = MakeBridgePair();
    BridgeCommandTransport command(pair.connection);
    BridgeDataReceiver data(pair.connection);
    REQUIRE(CompleteHandshake(command, *pair.peer).success);
    REQUIRE(data.Open(
        "mvlc-test", static_cast<std::uint16_t>(CommandPort + 1U)).success);

    data.Close();
    const auto payload = Bytes({0x44U, 0x55U});
    CHECK_FALSE(data.Send(payload.data(), payload.size()).success);
    REQUIRE(command.Send(payload.data(), payload.size()).success);

    const auto request = pair.peer->ReceiveFrame();
    INFO(request.error);
    REQUIRE(request.success);
    CHECK(request.frame.channel == BridgeChannel::Command);
    CHECK(request.frame.payload == payload);

    command.Close();
    CHECK(pair.peer->WaitForEof());
    CHECK_FALSE(command.Send(payload.data(), payload.size()).success);
}

TEST_CASE("a closed bridge peer wakes pending receives")
{
    using namespace fidget;

    auto pair = MakeBridgePair();
    BridgeCommandTransport command(pair.connection);
    REQUIRE(CompleteHandshake(command, *pair.peer).success);

    pair.peer->Close();
    std::array<std::byte, 16U> buffer{};
    const auto received = command.Receive(
        buffer.data(), buffer.size(), 1000);
    CHECK(received.status == TransportReceiveStatus::Error);
    CHECK(received.error == "Bridge peer closed the stream");
}
