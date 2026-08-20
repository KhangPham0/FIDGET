#include "core/BridgeProtocol.h"

#include <algorithm>
#include <utility>

namespace fidget {
namespace {

bool IsKnownBridgeChannel(const BridgeChannel channel)
{
    return channel == BridgeChannel::Command
        || channel == BridgeChannel::Data
        || channel == BridgeChannel::Hello;
}

std::uint32_t LoadBigEndian32(const std::byte* source)
{
    return (std::to_integer<std::uint32_t>(source[0]) << 24U)
        | (std::to_integer<std::uint32_t>(source[1]) << 16U)
        | (std::to_integer<std::uint32_t>(source[2]) << 8U)
        | std::to_integer<std::uint32_t>(source[3]);
}

void AppendBigEndian32(
    std::vector<std::byte>& destination, const std::uint32_t value)
{
    destination.push_back(static_cast<std::byte>((value >> 24U) & 0xFFU));
    destination.push_back(static_cast<std::byte>((value >> 16U) & 0xFFU));
    destination.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
    destination.push_back(static_cast<std::byte>(value & 0xFFU));
}

} // namespace

BridgeFrameEncodeResult EncodeBridgeFrame(
    const BridgeChannel channel,
    const std::byte* payload,
    const std::size_t payloadSize)
{
    BridgeFrameEncodeResult result;
    if (!IsKnownBridgeChannel(channel))
    {
        result.error = "Unknown bridge frame channel";
        return result;
    }
    if (payloadSize > BridgeMaximumPayloadSize)
    {
        result.error = "Bridge frame payload exceeds 65536 bytes";
        return result;
    }
    if (payloadSize > 0U && payload == nullptr)
    {
        result.error = "Bridge frame payload pointer is null";
        return result;
    }
    if (channel == BridgeChannel::Hello && payloadSize != 1U)
    {
        result.error = "Bridge hello must contain one protocol-version byte";
        return result;
    }
    if (channel == BridgeChannel::Hello
        && std::to_integer<std::uint8_t>(payload[0])
            != BridgeProtocolVersion)
    {
        result.error = "Unsupported bridge protocol version";
        return result;
    }

    result.bytes.reserve(BridgeFrameHeaderSize + payloadSize);
    result.bytes.push_back(static_cast<std::byte>(
        static_cast<std::uint8_t>(channel)));
    AppendBigEndian32(
        result.bytes, static_cast<std::uint32_t>(payloadSize));
    if (payloadSize > 0U)
    {
        result.bytes.insert(
            result.bytes.end(), payload, payload + payloadSize);
    }
    result.success = true;
    return result;
}

BridgeFrameEncodeResult EncodeBridgeHelloFrame()
{
    const std::byte version = static_cast<std::byte>(BridgeProtocolVersion);
    return EncodeBridgeFrame(BridgeChannel::Hello, &version, 1U);
}

BridgeFrameDecodeResult BridgeFrameDecoder::Consume(
    const std::byte* data, const std::size_t size)
{
    BridgeFrameDecodeResult result;
    if (failed_)
    {
        result.success = false;
        result.error = error_;
        return result;
    }
    if (size > 0U && data == nullptr)
    {
        Fail(result, "Bridge input pointer is null");
        return result;
    }

    std::size_t offset = 0U;
    while (offset < size)
    {
        if (headerSize_ < BridgeFrameHeaderSize)
        {
            const std::size_t count = std::min(
                BridgeFrameHeaderSize - headerSize_, size - offset);
            std::copy_n(
                data + offset, count, header_.data() + headerSize_);
            headerSize_ += count;
            offset += count;
            if (headerSize_ < BridgeFrameHeaderSize)
            {
                continue;
            }
            if (!BeginFrame(result))
            {
                return result;
            }
            if (payloadSize_ == 0U)
            {
                if (!FinishFrame(result))
                {
                    return result;
                }
                continue;
            }
        }

        const std::size_t count = std::min(
            payloadSize_ - payload_.size(), size - offset);
        payload_.insert(payload_.end(), data + offset, data + offset + count);
        offset += count;
        if (payload_.size() == payloadSize_)
        {
            if (!FinishFrame(result))
            {
                return result;
            }
        }
    }

    return result;
}

bool BridgeFrameDecoder::HelloReceived() const noexcept
{
    return helloReceived_;
}

bool BridgeFrameDecoder::Failed() const noexcept
{
    return failed_;
}

const std::string& BridgeFrameDecoder::Error() const noexcept
{
    return error_;
}

bool BridgeFrameDecoder::BeginFrame(BridgeFrameDecodeResult& result)
{
    const auto rawChannel = std::to_integer<std::uint8_t>(header_[0]);
    channel_ = static_cast<BridgeChannel>(rawChannel);
    if (!IsKnownBridgeChannel(channel_))
    {
        return Fail(result, "Unknown bridge frame channel");
    }

    payloadSize_ = LoadBigEndian32(header_.data() + 1U);
    if (payloadSize_ > BridgeMaximumPayloadSize)
    {
        return Fail(result, "Bridge frame payload exceeds 65536 bytes");
    }
    if (!helloReceived_ && channel_ != BridgeChannel::Hello)
    {
        return Fail(result, "Bridge hello must be the first frame");
    }
    if (helloReceived_ && channel_ == BridgeChannel::Hello)
    {
        return Fail(result, "Bridge hello may only appear once");
    }
    if (channel_ == BridgeChannel::Hello && payloadSize_ != 1U)
    {
        return Fail(
            result, "Bridge hello must contain one protocol-version byte");
    }

    payload_.clear();
    payload_.reserve(payloadSize_);
    return true;
}

bool BridgeFrameDecoder::FinishFrame(BridgeFrameDecodeResult& result)
{
    if (channel_ == BridgeChannel::Hello
        && std::to_integer<std::uint8_t>(payload_[0])
            != BridgeProtocolVersion)
    {
        return Fail(result, "Unsupported bridge protocol version");
    }

    BridgeFrame frame;
    frame.channel = channel_;
    frame.payload = std::move(payload_);
    result.frames.push_back(std::move(frame));

    if (channel_ == BridgeChannel::Hello)
    {
        helloReceived_ = true;
    }

    headerSize_ = 0U;
    payloadSize_ = 0U;
    payload_.clear();
    return true;
}

bool BridgeFrameDecoder::Fail(
    BridgeFrameDecodeResult& result, std::string error)
{
    failed_ = true;
    error_ = std::move(error);
    result.success = false;
    result.error = error_;
    return false;
}

} // namespace fidget
