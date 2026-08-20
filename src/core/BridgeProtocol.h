#ifndef FIDGET_CORE_BRIDGE_PROTOCOL_H
#define FIDGET_CORE_BRIDGE_PROTOCOL_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fidget {

enum class BridgeChannel : std::uint8_t
{
    Command = 0x00U,
    Data = 0x01U,
    Hello = 0x7FU,
};

inline constexpr std::uint8_t BridgeProtocolVersion = 1U;
inline constexpr std::size_t BridgeFrameHeaderSize = 5U;
inline constexpr std::size_t BridgeMaximumPayloadSize = 64U * 1024U;

struct BridgeFrame
{
    BridgeChannel channel = BridgeChannel::Command;
    std::vector<std::byte> payload;

    friend bool operator==(const BridgeFrame& left,
                           const BridgeFrame& right) noexcept
    {
        return left.channel == right.channel
            && left.payload == right.payload;
    }
};

struct BridgeFrameEncodeResult
{
    bool success = false;
    std::vector<std::byte> bytes;
    std::string error;
};

struct BridgeFrameDecodeResult
{
    bool success = true;
    std::vector<BridgeFrame> frames;
    std::string error;
};

[[nodiscard]] BridgeFrameEncodeResult EncodeBridgeFrame(
    BridgeChannel channel,
    const std::byte* payload,
    std::size_t payloadSize);

[[nodiscard]] BridgeFrameEncodeResult EncodeBridgeHelloFrame();

// A decoder represents one direction of one bridge stream. Stream corruption
// is terminal because there is no safe boundary at which decoding can resume.
class BridgeFrameDecoder
{
public:
    [[nodiscard]] BridgeFrameDecodeResult Consume(
        const std::byte* data,
        std::size_t size);

    [[nodiscard]] bool HelloReceived() const noexcept;
    [[nodiscard]] bool Failed() const noexcept;
    [[nodiscard]] const std::string& Error() const noexcept;

private:
    bool BeginFrame(BridgeFrameDecodeResult& result);
    bool FinishFrame(BridgeFrameDecodeResult& result);
    bool Fail(BridgeFrameDecodeResult& result, std::string error);

    std::array<std::byte, BridgeFrameHeaderSize> header_{};
    std::size_t headerSize_ = 0U;
    BridgeChannel channel_ = BridgeChannel::Command;
    std::size_t payloadSize_ = 0U;
    std::vector<std::byte> payload_;
    bool helloReceived_ = false;
    bool failed_ = false;
    std::string error_;
};

} // namespace fidget

#endif
