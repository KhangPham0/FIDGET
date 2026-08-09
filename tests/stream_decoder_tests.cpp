#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/StreamDecoder.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

std::uint32_t encodeSigned14(const int value)
{
    return static_cast<std::uint32_t>(value) & 0x3FFFU;
}

std::uint32_t packSamples(const int even, const int odd)
{
    return 0x30000000U | encodeSigned14(even)
        | (encodeSigned14(odd) << 14U);
}

void appendLittleEndianWord(
    std::vector<std::byte>& bytes, const std::uint32_t word)
{
    bytes.push_back(static_cast<std::byte>(word & 0xFFU));
    bytes.push_back(static_cast<std::byte>((word >> 8U) & 0xFFU));
    bytes.push_back(static_cast<std::byte>((word >> 16U) & 0xFFU));
    bytes.push_back(static_cast<std::byte>((word >> 24U) & 0xFFU));
}

std::vector<std::byte> makeWaveformPacket(
    const std::uint16_t packetNumber,
    const std::uint8_t sampleSource,
    const int sampleValue,
    const std::uint8_t moduleId = 0x11U,
    const int channel = 5)
{
    const std::array<std::uint32_t, 10> words = {
        0x20000008U
            | (static_cast<std::uint32_t>(packetNumber) << 16U),
        0x00000000U,
        0xF3010007U,
        0xF5200006U,
        0x41000005U | (static_cast<std::uint32_t>(moduleId) << 16U),
        0x10000000U | (static_cast<std::uint32_t>(channel) << 16U),
        0x30000001U
            | (static_cast<std::uint32_t>(sampleSource) << 19U),
        packSamples(sampleValue, -sampleValue),
        0x20000000U,
        0xC0000000U | packetNumber,
    };

    std::vector<std::byte> bytes;
    bytes.reserve(words.size() * sizeof(std::uint32_t));

    for (const auto word : words)
    {
        appendLittleEndianWord(bytes, word);
    }

    return bytes;
}

} // namespace

TEST_CASE("the stream decoder reconstructs waveforms across receive boundaries")
{
    using namespace fidget;

    // One synthetic Ethernet packet containing one F3 stack frame, one F5
    // module block and one four-sample MDPP-32 SCP trail on channel 26.
    const std::array<std::uint32_t, 11> words = {
        0x20010009U, // ETH header0: data channel, packet 1, 9 payload words
        0x00000000U, // ETH header1: next MVLC header at payload offset 0
        0xF3010008U, // F3 stack frame, 8 payload words
        0xF5200007U, // F5 block frame, 7 data words
        0x41110006U, // MDPP header, module ID 0x11
        0x101A1234U, // channel datum, channel 26
        0x30080002U, // sample source 1, two packed words/four samples
        packSamples(-2, 10),
        packSamples(100, -100),
        0x20000001U, // extended timestamp
        0xC0000012U, // low timestamp
    };

    std::vector<std::byte> bytes;
    bytes.reserve(words.size() * sizeof(std::uint32_t));
    for (const auto word : words)
    {
        appendLittleEndianWord(bytes, word);
    }

    MvmeStreamDecoder decoder;

    // Deliberately split words and packet boundaries like a real TCP stream.
    constexpr std::array<std::size_t, 6> chunkSizes = {1, 2, 5, 3, 11, 22};
    std::size_t offset = 0;

    for (const auto requestedSize : chunkSizes)
    {
        if (offset >= bytes.size())
        {
            break;
        }

        const std::size_t size =
            std::min(requestedSize, bytes.size() - offset);
        decoder.Consume(bytes.data() + offset, size);
        offset += size;
    }

    if (offset < bytes.size())
    {
        decoder.Consume(bytes.data() + offset, bytes.size() - offset);
    }

    const auto firstWaveform = decoder.LatestWaveform();
    REQUIRE(firstWaveform.has_value());

    const std::array<std::int16_t, 4> expectedSamples = {-2, 10, 100, -100};
    CHECK(firstWaveform->moduleId == 0x11U);
    CHECK(firstWaveform->channel == 26);
    CHECK(firstWaveform->sampleSource == 1U);
    CHECK(firstWaveform->samples.size() == 4U);
    CHECK(std::equal(expectedSamples.begin(),
                     expectedSamples.end(),
                     firstWaveform->samples.begin(),
                     firstWaveform->samples.end()));

    // A later event from another module must not overwrite the stored
    // waveform for module 0x11/channel 26.
    const std::array<std::uint32_t, 11> secondWords = {
        0x20020009U, // ETH data packet 2
        0x00000000U,
        0xF3010008U,
        0xF5200007U,
        0x41330006U, // module ID 0x33
        0x101A5678U, // channel 26
        0x30100002U, // sample source 2, four samples
        packSamples(1, 2),
        packSamples(3, 4),
        0x20000002U,
        0xC0000034U,
    };

    std::vector<std::byte> secondBytes;
    for (const auto word : secondWords)
    {
        appendLittleEndianWord(secondBytes, word);
    }

    decoder.Consume(secondBytes.data(), secondBytes.size());

    const auto latestWaveform = decoder.LatestWaveform();
    const auto firstStored =
        decoder.WaveformFor(MdppChannelAddress{0x11U, 26});
    const auto secondStored =
        decoder.WaveformFor(MdppChannelAddress{0x33U, 26});
    const auto firstHistory =
        decoder.HistoryFor(MdppChannelAddress{0x11U, 26});
    const auto availableChannels = decoder.AvailableChannels();
    const auto stats = decoder.Stats();

    REQUIRE(latestWaveform.has_value());
    REQUIRE(firstStored.has_value());
    REQUIRE(secondStored.has_value());
    REQUIRE(firstHistory.has_value());
    CHECK(latestWaveform->moduleId == 0x33U);
    CHECK(latestWaveform->sampleSource == 2U);
    CHECK(firstStored->sequence == firstWaveform->sequence);
    CHECK(firstStored->samples == firstWaveform->samples);
    CHECK(firstHistory->totalCaptured == 1U);
    CHECK(firstHistory->capacity == MdppWaveformHistoryCapacity);
    CHECK(firstHistory->waveforms.size() == 1U);
    REQUIRE(availableChannels.size() == 2U);
    CHECK((availableChannels[0] == MdppChannelAddress{0x11U, 26}));
    CHECK((availableChannels[1] == MdppChannelAddress{0x33U, 26}));
    CHECK(stats.ethernetPackets == 2U);
    CHECK(stats.decodedWaveforms == 2U);
    CHECK(stats.lostEthernetPackets == 0U);
    CHECK(stats.malformedWords == 0U);
}

TEST_CASE("a requested channel keeps the first observed module identity")
{
    using namespace fidget;

    // Direct acquisition must discover only the module ID. A hit on channel 0
    // must not move a user's preselected channel 29.
    const std::array firstObservedChannels{
        MdppChannelAddress{0x11U, 0},
        MdppChannelAddress{0x11U, 4}};
    const auto waitingTarget = ResolveMdppRequestedChannelTarget(
        firstObservedChannels.data(), firstObservedChannels.size(), -1, 29);
    CHECK(waitingTarget.moduleId == 0x11);
    CHECK(waitingTarget.requestedChannel == 29);
    CHECK(waitingTarget.moduleObserved);
    CHECK_FALSE(waitingTarget.channelObserved);

    const std::array laterObservedChannels{
        MdppChannelAddress{0x11U, 0},
        MdppChannelAddress{0x11U, 29},
        MdppChannelAddress{0x33U, 29}};
    const auto readyTarget = ResolveMdppRequestedChannelTarget(
        laterObservedChannels.data(),
        laterObservedChannels.size(),
        waitingTarget.moduleId,
        29);
    CHECK(readyTarget.moduleId == 0x11);
    CHECK(readyTarget.requestedChannel == 29);
    CHECK(readyTarget.moduleObserved);
    CHECK(readyTarget.channelObserved);

    const auto emptyTarget =
        ResolveMdppRequestedChannelTarget(nullptr, 0, -1, 31);
    CHECK_FALSE(emptyTarget.moduleObserved);
    CHECK_FALSE(emptyTarget.channelObserved);
    CHECK(emptyTarget.requestedChannel == 31);
}

TEST_CASE("waveform histories stay bounded and reset on configuration changes")
{
    using namespace fidget;

    MvmeStreamDecoder decoder;

    constexpr std::size_t eventsToGenerate =
        MdppWaveformHistoryCapacity + 5U;

    for (std::size_t event = 1; event <= eventsToGenerate; ++event)
    {
        const auto packet = makeWaveformPacket(
            static_cast<std::uint16_t>(event),
            1U,
            static_cast<int>(event));
        decoder.Consume(packet.data(), packet.size());
    }

    const MdppChannelAddress address{0x11U, 5};
    const auto history = decoder.HistoryFor(address);

    REQUIRE(history.has_value());
    CHECK(history->totalCaptured == eventsToGenerate);
    CHECK(history->waveforms.size() == MdppWaveformHistoryCapacity);
    CHECK(history->waveforms.front().sequence == 6U);
    CHECK(history->waveforms.back().sequence == eventsToGenerate);
    CHECK(history->waveforms.front().samples.front() == 6);
    CHECK(history->waveforms.back().samples.front()
          == static_cast<std::int16_t>(eventsToGenerate));

    // A source or sampling-format change starts a fresh compatible buffer,
    // while the total captured count remains a since-connect counter.
    const auto changedSourcePacket = makeWaveformPacket(
        static_cast<std::uint16_t>(eventsToGenerate + 1U), 2U, 200);
    decoder.Consume(changedSourcePacket.data(), changedSourcePacket.size());

    const auto changedHistory = decoder.HistoryFor(address);
    REQUIRE(changedHistory.has_value());
    CHECK(changedHistory->totalCaptured == eventsToGenerate + 1U);
    CHECK(changedHistory->waveforms.size() == 1U);
    CHECK(changedHistory->waveforms.front().sampleSource == 2U);
}

TEST_CASE("waveform histories cover every channel and support frozen snapshots")
{
    using namespace fidget;

    // Histories are collected for all sampled channels, independent of which
    // channel the GUI happens to display.
    MvmeStreamDecoder decoder;

    for (int channel = 0; channel < 32; ++channel)
    {
        const auto packet = makeWaveformPacket(
            static_cast<std::uint16_t>(channel + 1),
            1U,
            channel + 10,
            0x22U,
            channel);
        decoder.Consume(packet.data(), packet.size());
    }

    const auto availableChannels = decoder.AvailableChannels();
    CHECK(availableChannels.size() == 32U);

    for (int channel = 0; channel < 32; ++channel)
    {
        const auto channelHistory =
            decoder.HistoryFor(MdppChannelAddress{0x22U, channel});

        REQUIRE(channelHistory.has_value());
        CHECK(channelHistory->totalCaptured == 1U);
        CHECK(channelHistory->waveforms.size() == 1U);
    }

    // An all-channel snapshot must be a deep copy. Later live events and a
    // decoder reset must not alter the frozen data used as a reference.
    const auto frozenHistories = decoder.AllHistories();
    const auto additionalPacket = makeWaveformPacket(
        33U, 1U, 999, 0x22U, 0);
    decoder.Consume(additionalPacket.data(), additionalPacket.size());

    const MdppChannelAddress firstAddress{0x22U, 0};
    const auto liveFirstHistory = decoder.HistoryFor(firstAddress);
    const auto frozenFirstHistory = frozenHistories.find(firstAddress);

    CHECK(frozenHistories.size() == 32U);
    REQUIRE(frozenFirstHistory != frozenHistories.end());
    CHECK(frozenFirstHistory->second.waveforms.size() == 1U);
    CHECK(frozenFirstHistory->second.waveforms.back().samples.front() == 10);
    REQUIRE(liveFirstHistory.has_value());
    CHECK(liveFirstHistory->waveforms.size() == 2U);
    CHECK(liveFirstHistory->waveforms.back().samples.front() == 999);

    // A live parameter change clears only the affected four-channel quad.
    // Keep the history keys so the GUI's module/channel selection remains
    // stable while fresh, post-change waveforms arrive.
    decoder.ClearWaveformHistoriesForQuad(0U);
    const auto clearedChannel = decoder.HistoryFor(firstAddress);
    const auto untouchedChannel =
        decoder.HistoryFor(MdppChannelAddress{0x22U, 4});
    REQUIRE(clearedChannel.has_value());
    CHECK(clearedChannel->waveforms.empty());
    CHECK(clearedChannel->totalCaptured == 0U);
    REQUIRE(untouchedChannel.has_value());
    CHECK(untouchedChannel->waveforms.size() == 1U);
    CHECK(untouchedChannel->totalCaptured == 1U);
    CHECK(decoder.AvailableChannels().size() == 32U);
    CHECK_FALSE(decoder.LatestWaveform().has_value());

    decoder.Reset();
    CHECK(frozenFirstHistory->second.waveforms.back().samples.front() == 10);
}
