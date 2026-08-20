#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/BridgeProtocol.h"
#include "hardware/BridgeRelay.h"

#include <array>
#include <arpa/inet.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <poll.h>
#include <string>
#include <sys/socket.h>
#include <sys/time.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {

void CloseDescriptor(int& descriptor)
{
    if (descriptor >= 0)
    {
        ::close(descriptor);
        descriptor = -1;
    }
}

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

void Append(
    std::vector<std::byte>& destination,
    const std::vector<std::byte>& source)
{
    destination.insert(
        destination.end(), source.begin(), source.end());
}

std::vector<std::byte> Encode(
    const fidget::BridgeChannel channel,
    const std::vector<std::byte>& payload)
{
    const auto encoded = fidget::EncodeBridgeFrame(
        channel, payload.data(), payload.size());
    REQUIRE(encoded.success);
    return encoded.bytes;
}

void WriteAll(const int descriptor, const std::vector<std::byte>& bytes)
{
    std::size_t written = 0U;
    while (written < bytes.size())
    {
        const ssize_t result = ::write(
            descriptor, bytes.data() + written, bytes.size() - written);
        if (result < 0 && errno == EINTR)
        {
            continue;
        }
        REQUIRE(result > 0);
        written += static_cast<std::size_t>(result);
    }
}

struct ReceivedDatagram
{
    std::vector<std::byte> payload;
    sockaddr_in sender{};
};

class LocalUdpPair
{
public:
    LocalUdpPair()
    {
        for (int attempt = 0; attempt < 128; ++attempt)
        {
            if (TryOpen())
            {
                return;
            }
        }
        FAIL("could not reserve adjacent localhost UDP ports");
    }

    ~LocalUdpPair()
    {
        CloseDescriptor(commandDescriptor_);
        CloseDescriptor(dataDescriptor_);
    }

    LocalUdpPair(const LocalUdpPair&) = delete;
    LocalUdpPair& operator=(const LocalUdpPair&) = delete;

    [[nodiscard]] std::uint16_t CommandPort() const noexcept
    {
        return commandPort_;
    }

    ReceivedDatagram ReceiveCommand()
    {
        return Receive(commandDescriptor_);
    }

    ReceivedDatagram ReceiveData()
    {
        return Receive(dataDescriptor_);
    }

    void SendCommand(
        const std::vector<std::byte>& payload,
        const sockaddr_in& destination)
    {
        Send(commandDescriptor_, payload, destination);
    }

    void SendData(
        const std::vector<std::byte>& payload,
        const sockaddr_in& destination)
    {
        Send(dataDescriptor_, payload, destination);
    }

private:
    bool TryOpen()
    {
        CloseDescriptor(commandDescriptor_);
        CloseDescriptor(dataDescriptor_);
        commandPort_ = 0U;

        commandDescriptor_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (commandDescriptor_ < 0)
        {
            return false;
        }

        sockaddr_in commandAddress{};
        commandAddress.sin_family = AF_INET;
        commandAddress.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        commandAddress.sin_port = 0U;
        if (::bind(
                commandDescriptor_,
                reinterpret_cast<const sockaddr*>(&commandAddress),
                sizeof(commandAddress)) != 0)
        {
            return false;
        }

        socklen_t commandAddressLength = sizeof(commandAddress);
        if (::getsockname(
                commandDescriptor_,
                reinterpret_cast<sockaddr*>(&commandAddress),
                &commandAddressLength) != 0)
        {
            return false;
        }
        commandPort_ = ntohs(commandAddress.sin_port);
        if (commandPort_ == 0U || commandPort_ == 0xFFFFU)
        {
            return false;
        }

        dataDescriptor_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (dataDescriptor_ < 0)
        {
            return false;
        }
        sockaddr_in dataAddress = commandAddress;
        dataAddress.sin_port = htons(
            static_cast<std::uint16_t>(commandPort_ + 1U));
        if (::bind(
                dataDescriptor_,
                reinterpret_cast<const sockaddr*>(&dataAddress),
                sizeof(dataAddress)) != 0)
        {
            return false;
        }

        const timeval timeout{2, 0};
        if (::setsockopt(
                commandDescriptor_,
                SOL_SOCKET,
                SO_RCVTIMEO,
                &timeout,
                sizeof(timeout)) != 0
            || ::setsockopt(
                dataDescriptor_,
                SOL_SOCKET,
                SO_RCVTIMEO,
                &timeout,
                sizeof(timeout)) != 0)
        {
            return false;
        }
        return true;
    }

    static ReceivedDatagram Receive(const int descriptor)
    {
        ReceivedDatagram datagram;
        datagram.payload.resize(fidget::BridgeMaximumPayloadSize);
        socklen_t senderLength = sizeof(datagram.sender);
        ssize_t received = -1;
        do
        {
            received = ::recvfrom(
                descriptor,
                datagram.payload.data(),
                datagram.payload.size(),
                0,
                reinterpret_cast<sockaddr*>(&datagram.sender),
                &senderLength);
        } while (received < 0 && errno == EINTR);
        REQUIRE(received >= 0);
        datagram.payload.resize(static_cast<std::size_t>(received));
        return datagram;
    }

    static void Send(
        const int descriptor,
        const std::vector<std::byte>& payload,
        const sockaddr_in& destination)
    {
        ssize_t sent = -1;
        do
        {
            sent = ::sendto(
                descriptor,
                payload.data(),
                payload.size(),
                0,
                reinterpret_cast<const sockaddr*>(&destination),
                sizeof(destination));
        } while (sent < 0 && errno == EINTR);
        REQUIRE(sent == static_cast<ssize_t>(payload.size()));
    }

    int commandDescriptor_ = -1;
    int dataDescriptor_ = -1;
    std::uint16_t commandPort_ = 0U;
};

class RelayHarness
{
public:
    explicit RelayHarness(const std::uint16_t commandPort)
    {
        std::array<int, 2> inputPipe{};
        std::array<int, 2> outputPipe{};
        REQUIRE(::pipe(inputPipe.data()) == 0);
        REQUIRE(::pipe(outputPipe.data()) == 0);
        inputReadDescriptor_ = inputPipe[0];
        inputWriteDescriptor_ = inputPipe[1];
        outputReadDescriptor_ = outputPipe[0];
        outputWriteDescriptor_ = outputPipe[1];

        worker_ = std::thread([this, commandPort] {
            result_ = fidget::RunBridgeRelay(
                "127.0.0.1",
                commandPort,
                inputReadDescriptor_,
                outputWriteDescriptor_);
        });
    }

    ~RelayHarness()
    {
        CloseInput();
        Join();
        CloseDescriptor(inputReadDescriptor_);
        CloseDescriptor(outputReadDescriptor_);
        CloseDescriptor(outputWriteDescriptor_);
    }

    RelayHarness(const RelayHarness&) = delete;
    RelayHarness& operator=(const RelayHarness&) = delete;

    void Send(const std::vector<std::byte>& bytes) const
    {
        WriteAll(inputWriteDescriptor_, bytes);
    }

    void CloseInput()
    {
        CloseDescriptor(inputWriteDescriptor_);
    }

    void Join()
    {
        if (worker_.joinable())
        {
            worker_.join();
        }
    }

    [[nodiscard]] int OutputDescriptor() const noexcept
    {
        return outputReadDescriptor_;
    }

    [[nodiscard]] const fidget::BridgeRelayResult& Result() const noexcept
    {
        return result_;
    }

private:
    int inputReadDescriptor_ = -1;
    int inputWriteDescriptor_ = -1;
    int outputReadDescriptor_ = -1;
    int outputWriteDescriptor_ = -1;
    std::thread worker_;
    fidget::BridgeRelayResult result_;
};

class FrameReader
{
public:
    explicit FrameReader(const int descriptor)
        : descriptor_(descriptor)
    {
    }

    fidget::BridgeFrame Next()
    {
        while (frames_.empty())
        {
            pollfd ready{};
            ready.fd = descriptor_;
            ready.events = POLLIN | POLLHUP;
            int pollResult = -1;
            do
            {
                pollResult = ::poll(&ready, 1U, 2000);
            } while (pollResult < 0 && errno == EINTR);
            REQUIRE(pollResult > 0);
            REQUIRE((ready.revents & (POLLIN | POLLHUP)) != 0);

            std::array<std::byte, 8192> buffer{};
            ssize_t bytesRead = -1;
            do
            {
                bytesRead = ::read(
                    descriptor_, buffer.data(), buffer.size());
            } while (bytesRead < 0 && errno == EINTR);
            REQUIRE(bytesRead > 0);

            const auto decoded = decoder_.Consume(
                buffer.data(), static_cast<std::size_t>(bytesRead));
            INFO(decoded.error);
            REQUIRE(decoded.success);
            for (const auto& frame : decoded.frames)
            {
                frames_.push_back(frame);
            }
        }

        auto frame = std::move(frames_.front());
        frames_.pop_front();
        return frame;
    }

private:
    int descriptor_ = -1;
    fidget::BridgeFrameDecoder decoder_;
    std::deque<fidget::BridgeFrame> frames_;
};

void CheckServerHello(const fidget::BridgeFrame& hello)
{
    CHECK(hello.channel == fidget::BridgeChannel::Hello);
    CHECK(hello.payload == Bytes({fidget::BridgeProtocolVersion}));
}

std::vector<std::byte> ClientHello()
{
    const auto hello = fidget::EncodeBridgeHelloFrame();
    REQUIRE(hello.success);
    return hello.bytes;
}

} // namespace

TEST_CASE("the bridge relays both UDP channels and preserves datagrams")
{
    LocalUdpPair udp;
    RelayHarness relay(udp.CommandPort());
    FrameReader output(relay.OutputDescriptor());
    CheckServerHello(output.Next());

    const auto firstCommand = Bytes({0x10U, 0x20U, 0x30U});
    const auto secondCommand = Bytes({0x40U, 0x50U});
    const auto redirect = Bytes({0xF1U, 0xF2U});
    auto input = ClientHello();
    Append(input, Encode(fidget::BridgeChannel::Command, firstCommand));
    Append(input, Encode(fidget::BridgeChannel::Command, secondCommand));
    Append(input, Encode(fidget::BridgeChannel::Data, redirect));
    relay.Send(input);

    const auto receivedFirst = udp.ReceiveCommand();
    const auto receivedSecond = udp.ReceiveCommand();
    const auto receivedRedirect = udp.ReceiveData();
    CHECK(receivedFirst.payload == firstCommand);
    CHECK(receivedSecond.payload == secondCommand);
    CHECK(receivedRedirect.payload == redirect);

    const auto commandReply = Bytes({0xAAU, 0xBBU, 0xCCU});
    udp.SendCommand(commandReply, receivedFirst.sender);
    const auto commandFrame = output.Next();
    CHECK(commandFrame.channel == fidget::BridgeChannel::Command);
    CHECK(commandFrame.payload == commandReply);

    const auto firstData = Bytes({0x01U, 0x02U, 0x03U, 0x04U});
    const auto secondData = Bytes({0x05U});
    udp.SendData(firstData, receivedRedirect.sender);
    udp.SendData(secondData, receivedRedirect.sender);
    const auto firstDataFrame = output.Next();
    const auto secondDataFrame = output.Next();
    CHECK(firstDataFrame.channel == fidget::BridgeChannel::Data);
    CHECK(firstDataFrame.payload == firstData);
    CHECK(secondDataFrame.channel == fidget::BridgeChannel::Data);
    CHECK(secondDataFrame.payload == secondData);

    relay.CloseInput();
    relay.Join();
    INFO(relay.Result().error);
    CHECK(relay.Result().success);
}

TEST_CASE("stdin EOF exits the bridge cleanly")
{
    LocalUdpPair udp;
    RelayHarness relay(udp.CommandPort());
    FrameReader output(relay.OutputDescriptor());
    CheckServerHello(output.Next());

    relay.CloseInput();
    relay.Join();

    INFO(relay.Result().error);
    CHECK(relay.Result().success);
}

TEST_CASE("a malformed input frame terminates the bridge")
{
    LocalUdpPair udp;
    RelayHarness relay(udp.CommandPort());
    FrameReader output(relay.OutputDescriptor());
    CheckServerHello(output.Next());

    auto input = ClientHello();
    Append(input, Bytes({
        0x02U,
        0x00U, 0x00U, 0x00U, 0x00U,
    }));
    relay.Send(input);
    relay.Join();

    CHECK_FALSE(relay.Result().success);
    CHECK(relay.Result().error
          == "bridge input: Unknown bridge frame channel");
}

TEST_CASE("an oversized input frame terminates the bridge")
{
    LocalUdpPair udp;
    RelayHarness relay(udp.CommandPort());
    FrameReader output(relay.OutputDescriptor());
    CheckServerHello(output.Next());

    auto input = ClientHello();
    Append(input, Bytes({
        0x00U,
        0x00U, 0x01U, 0x00U, 0x01U,
    }));
    relay.Send(input);
    relay.Join();

    CHECK_FALSE(relay.Result().success);
    CHECK(relay.Result().error
          == "bridge input: Bridge frame payload exceeds 65536 bytes");
}

TEST_CASE("command port 65535 is rejected before opening sockets")
{
    const auto result = fidget::RunBridgeRelay(
        "127.0.0.1", 0xFFFFU, -1, -1);

    CHECK_FALSE(result.success);
    CHECK(result.error
          == "The MVLC command port has no adjacent data port.");
}
