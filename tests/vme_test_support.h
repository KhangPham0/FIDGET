#ifndef FIDGET_TESTS_VME_TEST_SUPPORT_H
#define FIDGET_TESTS_VME_TEST_SUPPORT_H

#include "core/VmeProtocol.h"
#include "fake_command_transport.h"

#include "doctest/doctest.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace fidget::test {

struct TransactionReferences
{
    std::uint16_t super = 0U;
    std::uint32_t stack = 0U;
};

struct WireOperation
{
    bool write = false;
    std::uint32_t address = 0U;
    std::uint16_t value = 0U;
};

inline std::vector<std::byte> EncodeWords(
    const std::vector<std::uint32_t>& words)
{
    return EncodeMvlcWordsLittleEndian(words.data(), words.size());
}

inline std::vector<std::byte> MakeCommandPacket(
    const std::vector<std::vector<std::uint32_t>>& frames)
{
    std::vector<std::uint32_t> words{0U, 0U};
    for (const auto& frame : frames)
    {
        words.insert(words.end(), frame.begin(), frame.end());
    }
    words[0] = static_cast<std::uint32_t>(words.size() - 2U);
    return EncodeWords(words);
}

inline std::vector<std::uint32_t> MakeSuperFrame(
    const std::uint16_t reference)
{
    return {
        (static_cast<std::uint32_t>(MvlcSuperFrameType) << 24U) | 1U,
        MvlcReferenceWordCommand | reference,
    };
}

inline std::vector<std::uint32_t> MakeReadStackFrame(
    const std::uint32_t stackReference,
    const std::uint16_t value)
{
    return {
        (static_cast<std::uint32_t>(MvlcStackFrameType) << 24U) | 2U,
        stackReference,
        value,
    };
}

inline std::vector<std::uint32_t> MakeWriteStackFrame(
    const std::uint32_t stackReference)
{
    return {
        (static_cast<std::uint32_t>(MvlcStackFrameType) << 24U) | 1U,
        stackReference,
    };
}

inline void QueueRead(
    FakeCommandTransport& transport,
    TransactionReferences& references,
    const std::uint32_t address,
    const std::uint16_t value)
{
    const auto operation = EncodeMvlcVmeReadD16Words(address);
    const auto upload = BuildMvlcStackUploadRequest(
        references.super,
        references.stack,
        operation.data(),
        operation.size());
    transport.QueueExchange({
        EncodeWords(upload),
        {FakeReceiveAction::Datagram(
            MakeCommandPacket({MakeSuperFrame(references.super)}))},
    });
    ++references.super;
    transport.QueueExchange({
        EncodeWords(BuildMvlcStackExecuteRequest(references.super)),
        {FakeReceiveAction::Datagram(MakeCommandPacket({
            MakeSuperFrame(references.super),
            MakeReadStackFrame(references.stack, value),
        }))},
    });
    ++references.super;
    ++references.stack;
}

inline void QueueWrite(
    FakeCommandTransport& transport,
    TransactionReferences& references,
    const std::uint32_t address,
    const std::uint16_t value)
{
    const auto operation = EncodeMvlcVmeWriteD16Words(address, value);
    const auto upload = BuildMvlcStackUploadRequest(
        references.super,
        references.stack,
        operation.data(),
        operation.size());
    transport.QueueExchange({
        EncodeWords(upload),
        {FakeReceiveAction::Datagram(
            MakeCommandPacket({MakeSuperFrame(references.super)}))},
    });
    ++references.super;
    transport.QueueExchange({
        EncodeWords(BuildMvlcStackExecuteRequest(references.super)),
        {FakeReceiveAction::Datagram(MakeCommandPacket({
            MakeSuperFrame(references.super),
            MakeWriteStackFrame(references.stack),
        }))},
    });
    ++references.super;
    ++references.stack;
}

inline std::vector<std::uint32_t> DecodeWords(
    const std::vector<std::byte>& bytes)
{
    REQUIRE(bytes.size() % sizeof(std::uint32_t) == 0U);
    std::vector<std::uint32_t> words;
    for (std::size_t offset = 0U;
         offset < bytes.size();
         offset += sizeof(std::uint32_t))
    {
        words.push_back(LoadLittleEndian32(bytes.data() + offset));
    }
    return words;
}

inline std::vector<WireOperation> DecodeWireOperations(
    const FakeCommandTransport& transport)
{
    std::vector<WireOperation> operations;
    for (const auto& request : transport.SentRequests())
    {
        const auto words = DecodeWords(request);
        for (std::size_t index = 0U; index < words.size(); ++index)
        {
            if (words[index] == MvlcVmeReadA32D16Command)
            {
                REQUIRE(index + 2U < words.size());
                operations.push_back({false, words[index + 2U], 0U});
            }
            else if (words[index] == MvlcVmeWriteA32D16Command)
            {
                REQUIRE(index + 4U < words.size());
                operations.push_back({
                    true,
                    words[index + 2U],
                    static_cast<std::uint16_t>(words[index + 4U]),
                });
            }
        }
    }
    return operations;
}

inline void Open(FakeCommandTransport& transport)
{
    REQUIRE(transport.Open("mvlc-test", 32768U).success);
}

} // namespace fidget::test

#endif
