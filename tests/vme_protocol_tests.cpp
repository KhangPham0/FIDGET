#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/VmeProtocol.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

std::vector<std::byte> MakePacket(
    std::uint32_t channel,
    const std::vector<std::uint32_t>& payload)
{
    std::vector<std::uint32_t> words{
        (channel << 28U) | static_cast<std::uint32_t>(payload.size()),
        0U,
    };
    words.insert(words.end(), payload.begin(), payload.end());
    return fidget::EncodeMvlcWordsLittleEndian(words.data(), words.size());
}

std::vector<std::uint32_t> MakeBatchFrame(
    std::uint16_t reference,
    const std::array<std::uint16_t, 3>& addresses,
    const std::array<std::uint32_t, 3>& values)
{
    std::vector<std::uint32_t> frame{
        (static_cast<std::uint32_t>(fidget::MvlcSuperFrameType) << 24U)
            | 7U,
        fidget::MvlcReferenceWordCommand | reference,
    };
    for (std::size_t index = 0U; index < addresses.size(); ++index)
    {
        frame.push_back(fidget::MvlcReadLocalCommand | addresses[index]);
        frame.push_back(values[index]);
    }
    return frame;
}

} // namespace

TEST_CASE("a local-register batch request preserves address order")
{
    constexpr std::array<std::uint16_t, 11> Addresses{
        0x1300U,
        0x1104U,
        0x1204U,
        0x2200U,
        0x2204U,
        0x2208U,
        0x220CU,
        0x2210U,
        0x2214U,
        0x2218U,
        0x221CU,
    };

    const auto request = fidget::BuildMvlcLocalRegisterBatchReadRequest(
        0x4567U, Addresses.data(), Addresses.size());

    REQUIRE(request.success);
    const std::vector<std::uint32_t> expected{
        0xF1000000U,
        0x01014567U,
        0x01021300U,
        0x01021104U,
        0x01021204U,
        0x01022200U,
        0x01022204U,
        0x01022208U,
        0x0102220CU,
        0x01022210U,
        0x01022214U,
        0x01022218U,
        0x0102221CU,
        0xF2000000U,
    };
    CHECK(request.words == expected);
}

TEST_CASE("an empty local-register batch is rejected")
{
    const auto request = fidget::BuildMvlcLocalRegisterBatchReadRequest(
        0x0001U, nullptr, 0U);

    CHECK_FALSE(request.success);
    CHECK(request.words.empty());
    CHECK(request.error == "No MVLC local registers were requested");
}

TEST_CASE("a batch parser skips interleaved unrelated reply frames")
{
    constexpr std::array<std::uint16_t, 3> Addresses{
        0x1104U,
        0x2200U,
        0x221CU,
    };
    constexpr std::array<std::uint32_t, 3> Values{
        0x00000100U,
        0xF3000000U,
        0xA5A55A5AU,
    };

    const auto stale = MakeBatchFrame(0x1111U, Addresses, Values);
    const auto matching = MakeBatchFrame(0x2222U, Addresses, Values);
    std::vector<std::uint32_t> payload = stale;
    payload.insert(payload.end(), matching.begin(), matching.end());
    const auto packet = MakePacket(0U, payload);

    const auto reply = fidget::ParseMvlcLocalRegisterBatchReadReply(
        packet.data(), packet.size(), 0x2222U,
        Addresses.data(), Addresses.size());

    REQUIRE(reply.status == fidget::MvlcLocalReadReplyStatus::Match);
    CHECK(reply.values
          == std::vector<std::uint32_t>(Values.begin(), Values.end()));
}

TEST_CASE("a channel-one packet does not match a local-register batch")
{
    constexpr std::array<std::uint16_t, 3> Addresses{
        0x1104U,
        0x2200U,
        0x221CU,
    };
    constexpr std::array<std::uint32_t, 3> Values{1U, 2U, 3U};
    const auto packet = MakePacket(
        1U, MakeBatchFrame(0x2222U, Addresses, Values));

    const auto reply = fidget::ParseMvlcLocalRegisterBatchReadReply(
        packet.data(), packet.size(), 0x2222U,
        Addresses.data(), Addresses.size());

    CHECK(reply.status == fidget::MvlcLocalReadReplyStatus::NoMatch);
    CHECK(reply.values.empty());
}

TEST_CASE("a matching batch with an unexpected mirrored read is malformed")
{
    constexpr std::array<std::uint16_t, 3> Addresses{
        0x1104U,
        0x2200U,
        0x221CU,
    };
    constexpr std::array<std::uint32_t, 3> Values{1U, 2U, 3U};
    auto frame = MakeBatchFrame(0x2222U, Addresses, Values);
    frame[4] = fidget::MvlcReadLocalCommand | 0x2204U;
    const auto packet = MakePacket(0U, frame);

    const auto reply = fidget::ParseMvlcLocalRegisterBatchReadReply(
        packet.data(), packet.size(), 0x2222U,
        Addresses.data(), Addresses.size());

    CHECK(reply.status == fidget::MvlcLocalReadReplyStatus::Malformed);
    CHECK(reply.values.empty());
}
