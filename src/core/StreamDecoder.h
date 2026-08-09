#ifndef FIDGET_CORE_STREAM_DECODER_H
#define FIDGET_CORE_STREAM_DECODER_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <vector>

namespace fidget {

struct MdppWaveform
{
    std::uint64_t sequence = 0;
    std::uint8_t moduleId = 0;
    int channel = -1;
    std::uint8_t sampleSource = 0;
    bool noOffsetCorrection = false;
    bool noResampling = false;
    std::uint16_t phase = 0;
    std::uint64_t timestamp = 0;
    std::vector<std::int16_t> samples;
};

struct StreamDecoderStats
{
    std::uint64_t systemFrames = 0;
    std::uint64_t ethernetPackets = 0;
    std::uint64_t lostEthernetPackets = 0;
    std::uint64_t blockFrames = 0;
    std::uint64_t decodedWaveforms = 0;
    std::uint64_t malformedWords = 0;
    std::size_t bufferedBytes = 0;
};

struct MdppChannelAddress
{
    std::uint8_t moduleId = 0;
    int channel = -1;

    friend bool operator<(const MdppChannelAddress& left,
                          const MdppChannelAddress& right) noexcept
    {
        if (left.moduleId != right.moduleId)
        {
            return left.moduleId < right.moduleId;
        }
        return left.channel < right.channel;
    }

    friend bool operator==(const MdppChannelAddress& left,
                           const MdppChannelAddress& right) noexcept
    {
        return left.moduleId == right.moduleId && left.channel == right.channel;
    }
};

// Resolves the data-derived module ID for a user-requested physical channel.
// Once a module ID has been observed it remains fixed. Hits on other channels
// never replace requestedChannel. This keeps direct-acquisition targeting
// deterministic while the selected channel is still waiting for its first hit.
struct MdppRequestedChannelTarget
{
    int moduleId = -1;
    int requestedChannel = -1;
    bool moduleObserved = false;
    bool channelObserved = false;
};

[[nodiscard]] MdppRequestedChannelTarget ResolveMdppRequestedChannelTarget(
    const MdppChannelAddress* availableChannels,
    std::size_t availableChannelCount,
    int currentModuleId,
    int requestedChannel);

inline constexpr std::size_t MdppWaveformHistoryCapacity = 100;

struct MdppChannelHistorySnapshot
{
    std::uint64_t totalCaptured = 0;
    std::size_t capacity = MdppWaveformHistoryCapacity;
    std::vector<MdppWaveform> waveforms;
};

using MdppChannelHistorySnapshots =
    std::map<MdppChannelAddress, MdppChannelHistorySnapshot>;

// Focused read-only decoder for the raw Ethernet MVLC stream produced by the
// MVME Stream Server. It reconstructs MVLC stack and block frames across
// arbitrary TCP receive boundaries and extracts sampled MDPP-32 SCP trails.
class MvmeStreamDecoder
{
public:
    void Reset();
    void ClearWaveformHistoriesForQuad(std::uint16_t selectedQuad);
    void Consume(const std::byte* data, std::size_t count);

    [[nodiscard]] std::optional<MdppWaveform> LatestWaveform() const;
    [[nodiscard]] std::optional<MdppWaveform> WaveformFor(
        MdppChannelAddress address) const;
    [[nodiscard]] std::optional<MdppChannelHistorySnapshot> HistoryFor(
        MdppChannelAddress address) const;
    [[nodiscard]] MdppChannelHistorySnapshots AllHistories() const;
    [[nodiscard]] std::vector<MdppChannelAddress> AvailableChannels() const;
    [[nodiscard]] StreamDecoderStats Stats() const;

private:
    static std::uint32_t ReadLittleEndianWord(const std::byte* data);
    static std::int16_t DecodeSigned14(std::uint32_t value);

    void ProcessPendingData();
    void ProcessEthernetPacket(const std::byte* packet, std::size_t packetWords);
    void ProcessStackWord(std::uint32_t word);
    void DecodeBlockFrame();
    void ResetFrameAssembly();

    struct ChannelHistory
    {
        std::uint64_t totalCaptured = 0;
        std::deque<MdppWaveform> waveforms;
    };

    mutable std::mutex mutex_;
    std::vector<std::byte> pendingBytes_;

    std::optional<std::uint16_t> lastEthernetPacket_;
    std::size_t stackWordsRemaining_ = 0;
    std::size_t blockWordsRemaining_ = 0;
    std::vector<std::uint32_t> blockData_;

    std::map<MdppChannelAddress, ChannelHistory> historiesByChannel_;
    std::optional<MdppChannelAddress> latestChannel_;
    StreamDecoderStats stats_;
    std::uint64_t nextWaveformSequence_ = 1;
};

} // namespace fidget

#endif
