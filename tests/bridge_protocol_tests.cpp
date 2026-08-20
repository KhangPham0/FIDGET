#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/BridgeProtocol.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <vector>

namespace {

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

} // namespace

TEST_CASE("bridge frames use a channel byte and big-endian length")
{
    std::vector<std::byte> payload(0x0102U, std::byte{0x5AU});
    const auto encoded = fidget::EncodeBridgeFrame(
        fidget::BridgeChannel::Command,
        payload.data(),
        payload.size());

    REQUIRE(encoded.success);
    REQUIRE(encoded.bytes.size()
            == fidget::BridgeFrameHeaderSize + payload.size());
    CHECK(encoded.bytes[0] == std::byte{0x00U});
    CHECK(encoded.bytes[1] == std::byte{0x00U});
    CHECK(encoded.bytes[2] == std::byte{0x00U});
    CHECK(encoded.bytes[3] == std::byte{0x01U});
    CHECK(encoded.bytes[4] == std::byte{0x02U});
    CHECK(encoded.bytes[5] == std::byte{0x5AU});
}

TEST_CASE("hello command and data frames round-trip")
{
    const auto hello = fidget::EncodeBridgeHelloFrame();
    REQUIRE(hello.success);
    CHECK(hello.bytes == Bytes({
        0x7FU,
        0x00U, 0x00U, 0x00U, 0x01U,
        fidget::BridgeProtocolVersion,
    }));
    const auto commandPayload = Bytes({0xF1U, 0x00U, 0xF2U});
    const auto dataPayload = Bytes({0x10U, 0x20U, 0x30U, 0x40U});

    std::vector<std::byte> stream = hello.bytes;
    Append(stream, Encode(fidget::BridgeChannel::Command, commandPayload));
    Append(stream, Encode(fidget::BridgeChannel::Data, dataPayload));

    fidget::BridgeFrameDecoder decoder;
    const auto decoded = decoder.Consume(stream.data(), stream.size());

    REQUIRE(decoded.success);
    REQUIRE(decoded.frames.size() == 3U);
    CHECK(decoded.frames[0].channel == fidget::BridgeChannel::Hello);
    CHECK(decoded.frames[0].payload
          == Bytes({fidget::BridgeProtocolVersion}));
    CHECK(decoded.frames[1].channel == fidget::BridgeChannel::Command);
    CHECK(decoded.frames[1].payload == commandPayload);
    CHECK(decoded.frames[2].channel == fidget::BridgeChannel::Data);
    CHECK(decoded.frames[2].payload == dataPayload);
    CHECK(decoder.HelloReceived());
}

TEST_CASE("a frame split across one-byte feeds is decoded once complete")
{
    const auto hello = fidget::EncodeBridgeHelloFrame();
    REQUIRE(hello.success);
    const auto payload = Bytes({0x01U, 0x23U, 0x45U, 0x67U, 0x89U});
    std::vector<std::byte> stream = hello.bytes;
    Append(stream, Encode(fidget::BridgeChannel::Command, payload));

    fidget::BridgeFrameDecoder decoder;
    std::vector<fidget::BridgeFrame> frames;
    for (std::size_t index = 0U; index < stream.size(); ++index)
    {
        const auto decoded = decoder.Consume(stream.data() + index, 1U);
        REQUIRE(decoded.success);
        frames.insert(
            frames.end(), decoded.frames.begin(), decoded.frames.end());
    }

    REQUIRE(frames.size() == 2U);
    CHECK(frames[0].channel == fidget::BridgeChannel::Hello);
    CHECK(frames[1].channel == fidget::BridgeChannel::Command);
    CHECK(frames[1].payload == payload);
}

TEST_CASE("two complete frames can arrive in one feed")
{
    const auto hello = fidget::EncodeBridgeHelloFrame();
    REQUIRE(hello.success);
    const auto payload = Bytes({0xAAU, 0xBBU});
    std::vector<std::byte> stream = hello.bytes;
    Append(stream, Encode(fidget::BridgeChannel::Data, payload));

    fidget::BridgeFrameDecoder decoder;
    const auto decoded = decoder.Consume(stream.data(), stream.size());

    REQUIRE(decoded.success);
    REQUIRE(decoded.frames.size() == 2U);
    CHECK(decoded.frames[0].channel == fidget::BridgeChannel::Hello);
    CHECK(decoded.frames[1].channel == fidget::BridgeChannel::Data);
    CHECK(decoded.frames[1].payload == payload);
}

TEST_CASE("the exact 64 KiB payload cap is accepted")
{
    std::vector<std::byte> payload(
        fidget::BridgeMaximumPayloadSize, std::byte{0xA5U});
    const auto encoded = fidget::EncodeBridgeFrame(
        fidget::BridgeChannel::Data, payload.data(), payload.size());

    REQUIRE(encoded.success);
    REQUIRE(encoded.bytes.size()
            == fidget::BridgeFrameHeaderSize + payload.size());
    CHECK(encoded.bytes[1] == std::byte{0x00U});
    CHECK(encoded.bytes[2] == std::byte{0x01U});
    CHECK(encoded.bytes[3] == std::byte{0x00U});
    CHECK(encoded.bytes[4] == std::byte{0x00U});

    const auto hello = fidget::EncodeBridgeHelloFrame();
    REQUIRE(hello.success);
    std::vector<std::byte> stream = hello.bytes;
    Append(stream, encoded.bytes);
    fidget::BridgeFrameDecoder decoder;
    const auto decoded = decoder.Consume(stream.data(), stream.size());

    REQUIRE(decoded.success);
    REQUIRE(decoded.frames.size() == 2U);
    CHECK(decoded.frames[1].channel == fidget::BridgeChannel::Data);
    CHECK(decoded.frames[1].payload == payload);
}

TEST_CASE("an oversized payload length is rejected from its header")
{
    const auto oversizedHeader = Bytes({
        0x00U,
        0x00U, 0x01U, 0x00U, 0x01U,
    });
    fidget::BridgeFrameDecoder decoder;

    const auto decoded = decoder.Consume(
        oversizedHeader.data(), oversizedHeader.size());

    CHECK_FALSE(decoded.success);
    CHECK(decoded.frames.empty());
    CHECK(decoded.error == "Bridge frame payload exceeds 65536 bytes");
    CHECK(decoder.Failed());
}

TEST_CASE("an unknown channel is rejected")
{
    const auto header = Bytes({
        0x02U,
        0x00U, 0x00U, 0x00U, 0x00U,
    });
    fidget::BridgeFrameDecoder decoder;

    const auto decoded = decoder.Consume(header.data(), header.size());

    CHECK_FALSE(decoded.success);
    CHECK(decoded.frames.empty());
    CHECK(decoded.error == "Unknown bridge frame channel");
}

TEST_CASE("a non-hello first frame is rejected")
{
    const auto command = Encode(fidget::BridgeChannel::Command, {});
    fidget::BridgeFrameDecoder decoder;

    const auto decoded = decoder.Consume(command.data(), command.size());

    CHECK_FALSE(decoded.success);
    CHECK(decoded.frames.empty());
    CHECK(decoded.error == "Bridge hello must be the first frame");
}

TEST_CASE("a repeated hello frame is rejected")
{
    const auto hello = fidget::EncodeBridgeHelloFrame();
    REQUIRE(hello.success);
    std::vector<std::byte> stream = hello.bytes;
    Append(stream, hello.bytes);
    fidget::BridgeFrameDecoder decoder;

    const auto decoded = decoder.Consume(stream.data(), stream.size());

    CHECK_FALSE(decoded.success);
    REQUIRE(decoded.frames.size() == 1U);
    CHECK(decoded.frames[0].channel == fidget::BridgeChannel::Hello);
    CHECK(decoded.error == "Bridge hello may only appear once");
}

TEST_CASE("an unsupported hello version is rejected")
{
    const auto frame = Bytes({
        0x7FU,
        0x00U, 0x00U, 0x00U, 0x01U,
        0x02U,
    });
    fidget::BridgeFrameDecoder decoder;

    const auto decoded = decoder.Consume(frame.data(), frame.size());

    CHECK_FALSE(decoded.success);
    CHECK(decoded.frames.empty());
    CHECK(decoded.error == "Unsupported bridge protocol version");
    CHECK_FALSE(decoder.HelloReceived());
}

TEST_CASE("a malformed hello payload length is rejected")
{
    const auto frame = Bytes({
        0x7FU,
        0x00U, 0x00U, 0x00U, 0x00U,
    });
    fidget::BridgeFrameDecoder decoder;

    const auto decoded = decoder.Consume(frame.data(), frame.size());

    CHECK_FALSE(decoded.success);
    CHECK(decoded.frames.empty());
    CHECK(decoded.error
          == "Bridge hello must contain one protocol-version byte");
}

TEST_CASE("a decoder failure is terminal")
{
    const auto badHeader = Bytes({
        0x02U,
        0x00U, 0x00U, 0x00U, 0x00U,
    });
    fidget::BridgeFrameDecoder decoder;
    REQUIRE_FALSE(decoder.Consume(
        badHeader.data(), badHeader.size()).success);
    const auto hello = fidget::EncodeBridgeHelloFrame();
    REQUIRE(hello.success);

    const auto afterFailure = decoder.Consume(
        hello.bytes.data(), hello.bytes.size());

    CHECK_FALSE(afterFailure.success);
    CHECK(afterFailure.frames.empty());
    CHECK(afterFailure.error == "Unknown bridge frame channel");
}
