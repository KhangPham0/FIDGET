#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "hardware/AcquisitionReceiver.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::uint32_t EncodeSigned14(const int value)
{
    return static_cast<std::uint32_t>(value) & 0x3FFFU;
}

std::uint32_t PackSamples(const int even, const int odd)
{
    return 0x30000000U | EncodeSigned14(even)
        | (EncodeSigned14(odd) << 14U);
}

void AppendLittleEndianWord(
    std::vector<std::byte>& bytes,
    const std::uint32_t word)
{
    bytes.push_back(static_cast<std::byte>(word & 0xFFU));
    bytes.push_back(static_cast<std::byte>((word >> 8U) & 0xFFU));
    bytes.push_back(static_cast<std::byte>((word >> 16U) & 0xFFU));
    bytes.push_back(static_cast<std::byte>((word >> 24U) & 0xFFU));
}

std::vector<std::byte> MakeWaveformPacket(
    const std::uint16_t packetNumber,
    const int channel,
    const int sampleValue)
{
    const std::array<std::uint32_t, 10> words{{
        0x20000008U
            | (static_cast<std::uint32_t>(packetNumber) << 16U),
        0x00000000U,
        0xF3010007U,
        0xF5200006U,
        0x41110005U,
        0x10000000U | (static_cast<std::uint32_t>(channel) << 16U),
        0x30080001U,
        PackSamples(sampleValue, -sampleValue),
        0x20000000U,
        0xC0000000U | packetNumber,
    }};
    std::vector<std::byte> bytes;
    for (const auto word : words)
    {
        AppendLittleEndianWord(bytes, word);
    }
    return bytes;
}

class ScriptedDataReceiver final : public fidget::IDataReceiver
{
public:
    void Queue(std::vector<std::byte> datagram)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        datagrams_.push_back(std::move(datagram));
    }

    [[nodiscard]] fidget::TransportOperationResult Open(
        const std::string&, std::uint16_t) override
    {
        return {true, {}};
    }

    [[nodiscard]] fidget::TransportOperationResult Send(
        const std::byte*, std::size_t) override
    {
        return {true, {}};
    }

    [[nodiscard]] fidget::TransportReceiveResult Receive(
        std::byte* buffer,
        const std::size_t capacity,
        int) override
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (!datagrams_.empty())
            {
                auto datagram = std::move(datagrams_.front());
                datagrams_.pop_front();
                if (datagram.size() > capacity)
                {
                    return {
                        fidget::TransportReceiveStatus::Error,
                        0U,
                        "scripted datagram is too large",
                    };
                }
                std::copy(datagram.begin(), datagram.end(), buffer);
                return {
                    fidget::TransportReceiveStatus::Received,
                    datagram.size(),
                    {},
                };
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return {
            fidget::TransportReceiveStatus::Timeout,
            0U,
            "data receive: timed out",
        };
    }

    void Close() noexcept override
    {
    }

private:
    std::mutex mutex_;
    std::deque<std::vector<std::byte>> datagrams_;
};

} // namespace

TEST_CASE("the receiver drains data and preserves the requested channel")
{
    using namespace fidget;

    ScriptedDataReceiver data;
    data.Queue(MakeWaveformPacket(1U, 0, 10));
    data.Queue(MakeWaveformPacket(2U, 29, 20));

    std::mutex publishedMutex;
    std::condition_variable publishedWakeup;
    DiagnosticStreamSnapshot published;
    AcquisitionReceiver receiver(data, std::chrono::milliseconds(5));
    REQUIRE(receiver.Start(
        29U,
        [&](const DiagnosticStreamSnapshot& next) {
            {
                const std::lock_guard<std::mutex> lock(publishedMutex);
                published = next;
            }
            publishedWakeup.notify_all();
        }));

    {
        std::unique_lock<std::mutex> lock(publishedMutex);
        REQUIRE(publishedWakeup.wait_for(
            lock,
            std::chrono::seconds(1),
            [&] {
                return published.decoderStats.decodedWaveforms == 2U;
            }));
    }

    receiver.StopAndJoin();
    const auto snapshot = receiver.CurrentSnapshot();
    CHECK_FALSE(receiver.IsRunning());
    CHECK_FALSE(snapshot.receiverRunning);
    CHECK(snapshot.receiverError.empty());
    CHECK(snapshot.datagramsReceived == 2U);
    CHECK(snapshot.bytesReceived == 80U);
    CHECK(snapshot.decoderStats.ethernetPackets == 2U);
    CHECK(snapshot.decoderStats.decodedWaveforms == 2U);
    CHECK(snapshot.decoderStats.lostEthernetPackets == 0U);
    CHECK(snapshot.decoderStats.malformedWords == 0U);
    CHECK(snapshot.requestedChannel == 29U);
    CHECK(snapshot.requestedTarget.moduleId == 0x11);
    CHECK(snapshot.requestedTarget.requestedChannel == 29);
    CHECK(snapshot.requestedTarget.moduleObserved);
    CHECK(snapshot.requestedTarget.channelObserved);
    REQUIRE(snapshot.histories.size() == 2U);
    const auto selected = snapshot.histories.find({0x11U, 29});
    REQUIRE(selected != snapshot.histories.end());
    REQUIRE(selected->second.waveforms.size() == 1U);
    CHECK(selected->second.waveforms.back().samples.front() == 20);
}
