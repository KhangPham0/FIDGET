#include "core/StreamDecoder.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace fidget {
namespace {

constexpr std::uint8_t MvlcStackFrame = 0xF3;
constexpr std::uint8_t MvlcBlockReadFrame = 0xF5;
constexpr std::uint8_t MvlcStackContinuation = 0xF9;
constexpr std::uint8_t MvlcSystemEvent = 0xFA;
constexpr std::uint8_t MvlcSystemEvent2 = 0xFB;

constexpr std::uint32_t FrameLengthMask = 0x1FFF;
constexpr std::uint32_t EthernetDataWordsMask = 0x1FFF;
constexpr std::uint32_t EthernetPacketNumberMask = 0x0FFF;
constexpr std::uint32_t EthernetNoHeaderPointer = 0x0FFF;
constexpr std::size_t EthernetHeaderWords = 2;

std::uint8_t FrameType(std::uint32_t word)
{
    return static_cast<std::uint8_t>(word >> 24);
}

bool HasSameSampleConfiguration(const MdppWaveform& left,
                                const MdppWaveform& right)
{
    return left.sampleSource == right.sampleSource
        && left.noOffsetCorrection == right.noOffsetCorrection
        && left.noResampling == right.noResampling
        && left.samples.size() == right.samples.size();
}

} // namespace

MdppRequestedChannelTarget ResolveMdppRequestedChannelTarget(
    const MdppChannelAddress* availableChannels,
    std::size_t availableChannelCount,
    int currentModuleId,
    int requestedChannel)
{
    MdppRequestedChannelTarget result;
    result.requestedChannel = requestedChannel;

    if (requestedChannel < 0 || requestedChannel >= 32)
    {
        return result;
    }

    // A direct tuner acquisition starts one configured MDPP. Discover its
    // event-header ID from the first waveform, then keep that identity stable
    // even if another channel or an unexpected module produces later hits.
    if (currentModuleId >= 0 && currentModuleId <= 0xFF)
    {
        result.moduleId = currentModuleId;
    }
    else if (availableChannelCount > 0)
    {
        result.moduleId = availableChannels[0].moduleId;
    }

    result.moduleObserved = result.moduleId >= 0;
    result.channelObserved = result.moduleObserved
        && availableChannelCount > 0
        && std::any_of(availableChannels,
                       availableChannels + availableChannelCount,
                       [&result](MdppChannelAddress address) {
                           return static_cast<int>(address.moduleId) == result.moduleId
                               && address.channel == result.requestedChannel;
                       });
    return result;
}

void MvmeStreamDecoder::Reset()
{
    const std::scoped_lock lock(mutex_);
    pendingBytes_.clear();
    lastEthernetPacket_.reset();
    ResetFrameAssembly();
    historiesByChannel_.clear();
    latestChannel_.reset();
    stats_ = {};
    nextWaveformSequence_ = 1;
}

void MvmeStreamDecoder::ClearWaveformHistoriesForQuad(
    std::uint16_t selectedQuad)
{
    const std::scoped_lock lock(mutex_);
    for (auto& entry : historiesByChannel_)
    {
        const MdppChannelAddress& address = entry.first;
        ChannelHistory& history = entry.second;
        if (address.channel >= 0
            && static_cast<std::uint16_t>(address.channel / 4) == selectedQuad)
        {
            history.totalCaptured = 0U;
            history.waveforms.clear();
        }
    }

    if (latestChannel_ && latestChannel_->channel >= 0
        && static_cast<std::uint16_t>(latestChannel_->channel / 4) == selectedQuad)
    {
        latestChannel_.reset();
    }
}

void MvmeStreamDecoder::Consume(const std::byte* data, std::size_t count)
{
    if (count == 0)
    {
        return;
    }

    const std::scoped_lock lock(mutex_);
    pendingBytes_.insert(pendingBytes_.end(), data, data + count);
    ProcessPendingData();
}

std::optional<MdppWaveform> MvmeStreamDecoder::LatestWaveform() const
{
    const std::scoped_lock lock(mutex_);

    if (!latestChannel_)
    {
        return std::nullopt;
    }

    const auto found = historiesByChannel_.find(*latestChannel_);
    if (found == historiesByChannel_.end() || found->second.waveforms.empty())
    {
        return std::nullopt;
    }

    return found->second.waveforms.back();
}

std::optional<MdppWaveform> MvmeStreamDecoder::WaveformFor(
    MdppChannelAddress address) const
{
    const std::scoped_lock lock(mutex_);
    const auto found = historiesByChannel_.find(address);

    if (found == historiesByChannel_.end() || found->second.waveforms.empty())
    {
        return std::nullopt;
    }

    return found->second.waveforms.back();
}

std::optional<MdppChannelHistorySnapshot> MvmeStreamDecoder::HistoryFor(
    MdppChannelAddress address) const
{
    const std::scoped_lock lock(mutex_);
    const auto found = historiesByChannel_.find(address);

    if (found == historiesByChannel_.end())
    {
        return std::nullopt;
    }

    MdppChannelHistorySnapshot result;
    result.totalCaptured = found->second.totalCaptured;
    result.waveforms.assign(found->second.waveforms.begin(),
                            found->second.waveforms.end());
    return result;
}

MdppChannelHistorySnapshots MvmeStreamDecoder::AllHistories() const
{
    const std::scoped_lock lock(mutex_);
    MdppChannelHistorySnapshots result;

    for (const auto& entry : historiesByChannel_)
    {
        MdppChannelHistorySnapshot snapshot;
        snapshot.totalCaptured = entry.second.totalCaptured;
        snapshot.waveforms.assign(entry.second.waveforms.begin(),
                                  entry.second.waveforms.end());
        result.emplace(entry.first, std::move(snapshot));
    }

    return result;
}

std::vector<MdppChannelAddress> MvmeStreamDecoder::AvailableChannels() const
{
    const std::scoped_lock lock(mutex_);
    std::vector<MdppChannelAddress> result;
    result.reserve(historiesByChannel_.size());

    for (const auto& entry : historiesByChannel_)
    {
        result.push_back(entry.first);
    }

    return result;
}

StreamDecoderStats MvmeStreamDecoder::Stats() const
{
    const std::scoped_lock lock(mutex_);
    auto result = stats_;
    result.bufferedBytes = pendingBytes_.size();
    return result;
}

std::uint32_t MvmeStreamDecoder::ReadLittleEndianWord(const std::byte* data)
{
    return std::to_integer<std::uint32_t>(data[0])
        | (std::to_integer<std::uint32_t>(data[1]) << 8U)
        | (std::to_integer<std::uint32_t>(data[2]) << 16U)
        | (std::to_integer<std::uint32_t>(data[3]) << 24U);
}

std::int16_t MvmeStreamDecoder::DecodeSigned14(std::uint32_t value)
{
    value &= 0x3FFFU;

    if ((value & 0x2000U) != 0U)
    {
        value |= 0xFFFFC000U;
    }

    return static_cast<std::int16_t>(value);
}

void MvmeStreamDecoder::ProcessPendingData()
{
    std::size_t offset = 0;

    while (pendingBytes_.size() - offset >= sizeof(std::uint32_t))
    {
        const std::uint32_t firstWord =
            ReadLittleEndianWord(pendingBytes_.data() + offset);
        const std::uint8_t type = FrameType(firstWord);

        if (type == MvlcSystemEvent || type == MvlcSystemEvent2)
        {
            const std::size_t frameWords =
                1U + static_cast<std::size_t>(firstWord & FrameLengthMask);
            const std::size_t frameBytes = frameWords * sizeof(std::uint32_t);

            if (pendingBytes_.size() - offset < frameBytes)
            {
                break;
            }

            ++stats_.systemFrames;
            lastEthernetPacket_.reset();
            ResetFrameAssembly();
            offset += frameBytes;
            continue;
        }

        // MVLC Ethernet header0 starts with the two-bit magic value 0b00.
        if ((firstWord >> 30U) == 0U)
        {
            if (pendingBytes_.size() - offset
                < EthernetHeaderWords * sizeof(std::uint32_t))
            {
                break;
            }

            const std::uint32_t packetChannel = (firstWord >> 28U) & 0x3U;
            if (packetChannel > 2U)
            {
                ++stats_.malformedWords;
                ResetFrameAssembly();
                offset += sizeof(std::uint32_t);
                continue;
            }

            const std::size_t packetWords = EthernetHeaderWords
                + static_cast<std::size_t>(firstWord & EthernetDataWordsMask);
            const std::size_t packetBytes = packetWords * sizeof(std::uint32_t);

            if (pendingBytes_.size() - offset < packetBytes)
            {
                break;
            }

            ProcessEthernetPacket(pendingBytes_.data() + offset, packetWords);
            offset += packetBytes;
            continue;
        }

        // Direct USB-style MVLC stack frames are inexpensive to support here
        // and make offline testing less transport-specific.
        if (type == MvlcStackFrame || type == MvlcStackContinuation)
        {
            const std::size_t frameWords =
                1U + static_cast<std::size_t>(firstWord & FrameLengthMask);
            const std::size_t frameBytes = frameWords * sizeof(std::uint32_t);

            if (pendingBytes_.size() - offset < frameBytes)
            {
                break;
            }

            for (std::size_t wordIndex = 0; wordIndex < frameWords; ++wordIndex)
            {
                ProcessStackWord(ReadLittleEndianWord(
                    pendingBytes_.data() + offset
                    + wordIndex * sizeof(std::uint32_t)));
            }

            offset += frameBytes;
            continue;
        }

        ++stats_.malformedWords;
        ResetFrameAssembly();
        offset += sizeof(std::uint32_t);
    }

    if (offset > 0)
    {
        pendingBytes_.erase(
            pendingBytes_.begin(),
            pendingBytes_.begin() + static_cast<std::ptrdiff_t>(offset));
    }
}

void MvmeStreamDecoder::ProcessEthernetPacket(const std::byte* packet,
                                              std::size_t packetWords)
{
    if (packetWords < EthernetHeaderWords)
    {
        return;
    }

    const std::uint32_t header0 = ReadLittleEndianWord(packet);
    const std::uint32_t header1 =
        ReadLittleEndianWord(packet + sizeof(std::uint32_t));
    const std::uint32_t packetChannel = (header0 >> 28U) & 0x3U;
    const std::uint16_t packetNumber = static_cast<std::uint16_t>(
        (header0 >> 16U) & EthernetPacketNumberMask);

    ++stats_.ethernetPackets;

    if (packetChannel != 2U)
    {
        return;
    }

    if (lastEthernetPacket_)
    {
        const std::uint16_t expected = static_cast<std::uint16_t>(
            (*lastEthernetPacket_ + 1U) & EthernetPacketNumberMask);

        if (packetNumber != expected)
        {
            const std::uint16_t lost = static_cast<std::uint16_t>(
                (packetNumber - expected) & EthernetPacketNumberMask);
            stats_.lostEthernetPackets += lost;
            ResetFrameAssembly();
        }
    }

    lastEthernetPacket_ = packetNumber;

    const std::size_t payloadWords = packetWords - EthernetHeaderWords;
    std::size_t payloadIndex = 0;

    if (stackWordsRemaining_ == 0)
    {
        const std::uint32_t nextHeaderPointer = header1 & 0x0FFFU;

        if (nextHeaderPointer == EthernetNoHeaderPointer)
        {
            return;
        }

        if (nextHeaderPointer >= payloadWords)
        {
            ++stats_.malformedWords;
            return;
        }

        payloadIndex = nextHeaderPointer;
    }

    const std::byte* payload =
        packet + EthernetHeaderWords * sizeof(std::uint32_t);

    for (; payloadIndex < payloadWords; ++payloadIndex)
    {
        ProcessStackWord(ReadLittleEndianWord(
            payload + payloadIndex * sizeof(std::uint32_t)));
    }
}

void MvmeStreamDecoder::ProcessStackWord(std::uint32_t word)
{
    if (blockWordsRemaining_ > 0)
    {
        if (stackWordsRemaining_ == 0)
        {
            ++stats_.malformedWords;
            ResetFrameAssembly();
            return;
        }

        blockData_.push_back(word);
        --blockWordsRemaining_;
        --stackWordsRemaining_;

        if (blockWordsRemaining_ == 0)
        {
            ++stats_.blockFrames;
            DecodeBlockFrame();
            blockData_.clear();
        }

        return;
    }

    if (stackWordsRemaining_ > 0)
    {
        if (FrameType(word) == MvlcBlockReadFrame)
        {
            const std::size_t blockLength = word & FrameLengthMask;
            --stackWordsRemaining_;

            if (blockLength > stackWordsRemaining_)
            {
                ++stats_.malformedWords;
                ResetFrameAssembly();
                return;
            }

            blockWordsRemaining_ = blockLength;
            blockData_.clear();
            blockData_.reserve(blockLength);

            if (blockLength == 0)
            {
                ++stats_.blockFrames;
            }
        }
        else
        {
            --stackWordsRemaining_;
        }

        return;
    }

    const std::uint8_t type = FrameType(word);
    if (type == MvlcStackFrame || type == MvlcStackContinuation)
    {
        stackWordsRemaining_ = word & FrameLengthMask;
    }
}

void MvmeStreamDecoder::DecodeBlockFrame()
{
    if (blockData_.size() < 3)
    {
        return;
    }

    const std::uint32_t moduleHeader = blockData_.front();
    if ((moduleHeader >> 28U) != 0x4U)
    {
        return;
    }

    const std::uint8_t moduleId =
        static_cast<std::uint8_t>((moduleHeader >> 16U) & 0xFFU);

    std::uint64_t timestamp = 0;
    std::uint16_t extendedTimestamp = 0;

    for (std::uint32_t word : blockData_)
    {
        if ((word >> 30U) == 0x3U)
        {
            timestamp = word & 0x3FFFFFFFU;
        }
        else if ((word >> 28U) == 0x2U)
        {
            extendedTimestamp = static_cast<std::uint16_t>(word & 0xFFFFU);
        }
    }

    timestamp |= static_cast<std::uint64_t>(extendedTimestamp) << 30U;

    int currentChannel = -1;

    for (std::size_t index = 1; index < blockData_.size(); ++index)
    {
        const std::uint32_t word = blockData_[index];
        const std::uint32_t topNibble = word >> 28U;

        if (topNibble == 0x1U)
        {
            // MDPP-32 SCP channel-time and amplitude words use type fields
            // 00 and 01 in bits 22:21. Trigger-time words use a different
            // type and are deliberately excluded.
            const std::uint32_t datumType = (word >> 21U) & 0x3U;
            if (datumType <= 1U)
            {
                currentChannel = static_cast<int>((word >> 16U) & 0x1FU);
            }
            continue;
        }

        if (topNibble != 0x3U || currentChannel < 0)
        {
            continue;
        }

        const std::size_t sampleWords = word & 0x03FFU;
        if (sampleWords == 0 || index + sampleWords >= blockData_.size())
        {
            ++stats_.malformedWords;
            continue;
        }

        const std::uint8_t config =
            static_cast<std::uint8_t>((word >> 19U) & 0xFFU);

        MdppWaveform waveform;
        waveform.sequence = nextWaveformSequence_++;
        waveform.moduleId = moduleId;
        waveform.channel = currentChannel;
        waveform.sampleSource = config & 0x3U;
        waveform.noOffsetCorrection = (config & 0x80U) != 0U;
        waveform.noResampling = (config & 0x40U) != 0U;
        waveform.phase = static_cast<std::uint16_t>((word >> 10U) & 0x01FFU);
        waveform.timestamp = timestamp;
        waveform.samples.reserve(sampleWords * 2U);

        bool validSamples = true;

        for (std::size_t sampleIndex = 0; sampleIndex < sampleWords; ++sampleIndex)
        {
            const std::uint32_t packedSamples =
                blockData_[index + 1U + sampleIndex];

            if ((packedSamples >> 28U) != 0x3U)
            {
                validSamples = false;
                break;
            }

            const std::uint32_t samplePayload = packedSamples & 0x0FFFFFFFU;
            waveform.samples.push_back(DecodeSigned14(samplePayload & 0x3FFFU));
            waveform.samples.push_back(
                DecodeSigned14((samplePayload >> 14U) & 0x3FFFU));
        }

        if (validSamples)
        {
            const MdppChannelAddress address{waveform.moduleId, waveform.channel};
            auto& history = historiesByChannel_[address];
            ++history.totalCaptured;

            if (!history.waveforms.empty()
                && !HasSameSampleConfiguration(history.waveforms.back(), waveform))
            {
                history.waveforms.clear();
            }

            if (history.waveforms.size() == MdppWaveformHistoryCapacity)
            {
                history.waveforms.pop_front();
            }

            latestChannel_ = address;
            history.waveforms.push_back(std::move(waveform));
            ++stats_.decodedWaveforms;
        }
        else
        {
            ++stats_.malformedWords;
        }

        index += sampleWords;
    }
}

void MvmeStreamDecoder::ResetFrameAssembly()
{
    stackWordsRemaining_ = 0;
    blockWordsRemaining_ = 0;
    blockData_.clear();
}

} // namespace fidget
