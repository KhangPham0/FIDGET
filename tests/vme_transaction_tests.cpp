#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/VmeProtocol.h"
#include "fake_command_transport.h"
#include "hardware/VmeTransaction.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace {

std::vector<std::byte> EncodeWords(const std::vector<std::uint32_t>& words)
{
    return fidget::EncodeMvlcWordsLittleEndian(words.data(), words.size());
}

std::vector<std::byte> MakeCommandPacket(
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

std::vector<std::uint32_t> MakeSuperFrame(
    std::uint16_t reference,
    std::uint8_t flags = 0U)
{
    return {
        (static_cast<std::uint32_t>(fidget::MvlcSuperFrameType) << 24U)
            | (static_cast<std::uint32_t>(flags) << 20U) | 1U,
        fidget::MvlcReferenceWordCommand | reference,
    };
}

std::vector<std::uint32_t> MakeReadStackFrame(
    std::uint32_t stackReference,
    std::uint16_t value,
    std::uint8_t flags = 0U)
{
    return {
        (static_cast<std::uint32_t>(fidget::MvlcStackFrameType) << 24U)
            | (static_cast<std::uint32_t>(flags) << 20U) | 2U,
        stackReference,
        value,
    };
}

std::vector<std::uint32_t> MakeWriteStackFrame(
    std::uint32_t stackReference,
    std::uint8_t flags = 0U)
{
    return {
        (static_cast<std::uint32_t>(fidget::MvlcStackFrameType) << 24U)
            | (static_cast<std::uint32_t>(flags) << 20U) | 1U,
        stackReference,
    };
}

std::vector<std::uint32_t> MakeStackErrorFrame()
{
    return {
        static_cast<std::uint32_t>(fidget::MvlcStackErrorFrameType) << 24U,
    };
}

std::vector<std::byte> MakeUploadRequest(
    std::uint16_t superReference,
    std::uint32_t stackReference,
    std::uint32_t address)
{
    const auto operation = fidget::EncodeMvlcVmeReadD16Words(address);
    const auto request = fidget::BuildMvlcStackUploadRequest(
        superReference,
        stackReference,
        operation.data(),
        operation.size());
    return EncodeWords(request);
}

std::vector<std::byte> MakeExecuteRequest(std::uint16_t superReference)
{
    return EncodeWords(fidget::BuildMvlcStackExecuteRequest(superReference));
}

std::vector<std::uint32_t> DecodeWords(
    const std::vector<std::byte>& bytes)
{
    REQUIRE(bytes.size() % sizeof(std::uint32_t) == 0U);
    std::vector<std::uint32_t> words;
    for (std::size_t offset = 0U;
         offset < bytes.size();
         offset += sizeof(std::uint32_t))
    {
        words.push_back(fidget::LoadLittleEndian32(bytes.data() + offset));
    }
    return words;
}

void Open(fidget::test::FakeCommandTransport& transport)
{
    REQUIRE(transport.Open("mvlc-test", 32768U).success);
}

} // namespace

TEST_CASE("a VME read succeeds on its first transaction attempt")
{
    using namespace fidget;
    using namespace fidget::test;

    constexpr std::uint32_t Address = 0x11006004U;
    constexpr std::uint16_t UploadReference = 0x1200U;
    constexpr std::uint16_t ExecuteReference = 0x1201U;
    constexpr std::uint32_t StackReference = 0x10203040U;

    FakeCommandTransport transport;
    Open(transport);
    transport.QueueExchange({
        MakeUploadRequest(UploadReference, StackReference, Address),
        {FakeReceiveAction::Datagram(
            MakeCommandPacket({MakeSuperFrame(UploadReference)}))},
    });
    transport.QueueExchange({
        MakeExecuteRequest(ExecuteReference),
        {FakeReceiveAction::Datagram(MakeCommandPacket({
            MakeSuperFrame(ExecuteReference),
            MakeReadStackFrame(StackReference, 0x2051U),
        }))},
    });

    std::uint16_t nextSuperReference = UploadReference;
    std::uint32_t nextStackReference = StackReference;
    const std::atomic<bool> cancelled{false};
    const auto result = ReadVmeD16(
        transport,
        Address,
        nextSuperReference,
        nextStackReference,
        cancelled);

    REQUIRE(result.success);
    CHECK(result.value == 0x2051U);
    CHECK(nextSuperReference == 0x1202U);
    CHECK(nextStackReference == 0x10203041U);
    CHECK(transport.SentRequests().size() == 2U);
    for (const auto capacity : transport.ReceiveCapacities())
    {
        CHECK(capacity == MvlcCommandResponseBufferSize);
    }
}

TEST_CASE("the upload scan consumes eight unrelated datagrams before retrying")
{
    using namespace fidget;
    using namespace fidget::test;

    constexpr std::uint32_t Address = 0x11006004U;
    constexpr std::uint16_t FirstUploadReference = 0x2000U;
    constexpr std::uint32_t FirstStackReference = 0xA0000000U;

    FakeCommandTransport transport;
    Open(transport);
    std::vector<FakeReceiveAction> unrelatedReplies;
    for (int index = 0; index < MvlcMaximumResponseDatagrams; ++index)
    {
        unrelatedReplies.push_back(FakeReceiveAction::Datagram(
            MakeCommandPacket({MakeSuperFrame(0x7777U)})));
    }
    transport.QueueExchange({
        MakeUploadRequest(
            FirstUploadReference, FirstStackReference, Address),
        std::move(unrelatedReplies),
    });
    transport.QueueExchange({
        MakeUploadRequest(
            FirstUploadReference + 1U,
            FirstStackReference + 1U,
            Address),
        {FakeReceiveAction::Datagram(MakeCommandPacket({
            MakeSuperFrame(FirstUploadReference + 1U),
        }))},
    });
    transport.QueueExchange({
        MakeExecuteRequest(FirstUploadReference + 2U),
        {FakeReceiveAction::Datagram(MakeCommandPacket({
            MakeSuperFrame(FirstUploadReference + 2U),
            MakeReadStackFrame(FirstStackReference + 1U, 0x5007U),
        }))},
    });

    std::uint16_t nextSuperReference = FirstUploadReference;
    std::uint32_t nextStackReference = FirstStackReference;
    const std::atomic<bool> cancelled{false};
    const auto result = ReadVmeD16(
        transport,
        Address,
        nextSuperReference,
        nextStackReference,
        cancelled);

    REQUIRE(result.success);
    CHECK(result.value == 0x5007U);
    CHECK(transport.ReceiveCapacities().size() == 10U);
    REQUIRE(transport.SentRequests().size() == 3U);

    const auto firstUpload = DecodeWords(transport.SentRequests()[0]);
    const auto secondUpload = DecodeWords(transport.SentRequests()[1]);
    const auto execute = DecodeWords(transport.SentRequests()[2]);
    REQUIRE(firstUpload.size() > 7U);
    REQUIRE(secondUpload.size() > 7U);
    REQUIRE(execute.size() > 1U);
    CHECK(firstUpload[1]
          == (MvlcReferenceWordCommand | FirstUploadReference));
    CHECK(firstUpload[7] == FirstStackReference);
    CHECK(secondUpload[1]
          == (MvlcReferenceWordCommand | (FirstUploadReference + 1U)));
    CHECK(secondUpload[7] == FirstStackReference + 1U);
    CHECK(execute[1]
          == (MvlcReferenceWordCommand | (FirstUploadReference + 2U)));
    CHECK(nextSuperReference == FirstUploadReference + 3U);
    CHECK(nextStackReference == FirstStackReference + 2U);
}

TEST_CASE("a missing upload response retries and keeps the final error")
{
    using namespace fidget;
    using namespace fidget::test;

    constexpr std::uint32_t Address = 0x11006004U;
    FakeCommandTransport transport;
    Open(transport);
    transport.QueueExchange({
        MakeUploadRequest(1U, 10U, Address),
        {FakeReceiveAction::Error("first upload failure")},
    });
    transport.QueueExchange({
        MakeUploadRequest(2U, 11U, Address),
        {FakeReceiveAction::Error("second upload failure")},
    });
    transport.QueueExchange({
        MakeUploadRequest(3U, 12U, Address),
        {FakeReceiveAction::Error("last upload failure")},
    });

    std::uint16_t nextSuperReference = 1U;
    std::uint32_t nextStackReference = 10U;
    const std::atomic<bool> cancelled{false};
    const auto result = ReadVmeD16(
        transport,
        Address,
        nextSuperReference,
        nextStackReference,
        cancelled);

    CHECK_FALSE(result.success);
    CHECK(result.error == "last upload failure");
    CHECK(transport.SentRequests().size() == 3U);
    CHECK(nextSuperReference == 4U);
    CHECK(nextStackReference == 13U);
}

TEST_CASE("VME bus errors and missing devices do not retry")
{
    using namespace fidget;
    using namespace fidget::test;

    constexpr std::uint32_t Address = 0x11006004U;
    constexpr std::uint32_t StackReference = 0x22220000U;

    const auto runTerminalFailure = [Address, StackReference](
                                        std::uint8_t flag,
                                        const char* expectedError) {
        FakeCommandTransport transport;
        Open(transport);
        transport.QueueExchange({
            MakeUploadRequest(100U, StackReference, Address),
            {FakeReceiveAction::Datagram(
                MakeCommandPacket({MakeSuperFrame(100U)}))},
        });
        transport.QueueExchange({
            MakeExecuteRequest(101U),
            {FakeReceiveAction::Datagram(MakeCommandPacket({
                MakeSuperFrame(101U),
                MakeReadStackFrame(StackReference, 0U, flag),
            }))},
        });

        std::uint16_t nextSuperReference = 100U;
        std::uint32_t nextStackReference = StackReference;
        const std::atomic<bool> cancelled{false};
        const auto result = ReadVmeD16(
            transport,
            Address,
            nextSuperReference,
            nextStackReference,
            cancelled);

        CHECK_FALSE(result.success);
        CHECK(result.error == expectedError);
        CHECK(transport.SentRequests().size() == 2U);
        CHECK(nextSuperReference == 102U);
        CHECK(nextStackReference == StackReference + 1U);
    };

    runTerminalFailure(MvlcBusErrorFlag, "VME bus error");
    runTerminalFailure(MvlcTimeoutFlag, "VME device did not respond");
}

TEST_CASE("a rejected stack upload retries all three attempts")
{
    using namespace fidget;
    using namespace fidget::test;

    constexpr std::uint32_t Address = 0x11006004U;
    FakeCommandTransport transport;
    Open(transport);
    for (std::uint16_t attempt = 0U;
         attempt < MvlcTransactionAttemptCount;
         ++attempt)
    {
        const std::uint16_t reference = 100U + attempt;
        transport.QueueExchange({
            MakeUploadRequest(reference, 1000U + attempt, Address),
            {FakeReceiveAction::Datagram(MakeCommandPacket({
                MakeSuperFrame(reference, MvlcSyntaxErrorFlag),
            }))},
        });
    }

    std::uint16_t nextSuperReference = 100U;
    std::uint32_t nextStackReference = 1000U;
    const std::atomic<bool> cancelled{false};
    const auto result = ReadVmeD16(
        transport,
        Address,
        nextSuperReference,
        nextStackReference,
        cancelled);

    CHECK_FALSE(result.success);
    CHECK(result.error == "MVLC rejected the transient VME stack");
    CHECK(transport.SentRequests().size() == 3U);
    CHECK(nextSuperReference == 103U);
    CHECK(nextStackReference == 1003U);
}

TEST_CASE("a rejected execute command retries all three attempts")
{
    using namespace fidget;
    using namespace fidget::test;

    constexpr std::uint32_t Address = 0x11006004U;
    FakeCommandTransport transport;
    Open(transport);
    for (std::uint16_t attempt = 0U;
         attempt < MvlcTransactionAttemptCount;
         ++attempt)
    {
        const std::uint16_t uploadReference =
            static_cast<std::uint16_t>(200U + attempt * 2U);
        const std::uint16_t executeReference = uploadReference + 1U;
        const std::uint32_t stackReference = 2000U + attempt;
        transport.QueueExchange({
            MakeUploadRequest(uploadReference, stackReference, Address),
            {FakeReceiveAction::Datagram(
                MakeCommandPacket({MakeSuperFrame(uploadReference)}))},
        });
        transport.QueueExchange({
            MakeExecuteRequest(executeReference),
            {FakeReceiveAction::Datagram(MakeCommandPacket({
                MakeSuperFrame(
                    executeReference, MvlcSyntaxErrorFlag),
            }))},
        });
    }

    std::uint16_t nextSuperReference = 200U;
    std::uint32_t nextStackReference = 2000U;
    const std::atomic<bool> cancelled{false};
    const auto result = ReadVmeD16(
        transport,
        Address,
        nextSuperReference,
        nextStackReference,
        cancelled);

    CHECK_FALSE(result.success);
    CHECK(result.error == "MVLC rejected the immediate-stack trigger");
    CHECK(transport.SentRequests().size() == 6U);
    CHECK(nextSuperReference == 206U);
    CHECK(nextStackReference == 2003U);
}

TEST_CASE("an F7 stack error retries all three attempts")
{
    using namespace fidget;
    using namespace fidget::test;

    constexpr std::uint32_t Address = 0x11006004U;
    FakeCommandTransport transport;
    Open(transport);
    for (std::uint16_t attempt = 0U;
         attempt < MvlcTransactionAttemptCount;
         ++attempt)
    {
        const std::uint16_t uploadReference =
            static_cast<std::uint16_t>(300U + attempt * 2U);
        const std::uint16_t executeReference = uploadReference + 1U;
        const std::uint32_t stackReference = 3000U + attempt;
        transport.QueueExchange({
            MakeUploadRequest(uploadReference, stackReference, Address),
            {FakeReceiveAction::Datagram(
                MakeCommandPacket({MakeSuperFrame(uploadReference)}))},
        });
        transport.QueueExchange({
            MakeExecuteRequest(executeReference),
            {FakeReceiveAction::Datagram(MakeCommandPacket({
                MakeSuperFrame(executeReference),
                MakeStackErrorFrame(),
            }))},
        });
    }

    std::uint16_t nextSuperReference = 300U;
    std::uint32_t nextStackReference = 3000U;
    const std::atomic<bool> cancelled{false};
    const auto result = ReadVmeD16(
        transport,
        Address,
        nextSuperReference,
        nextStackReference,
        cancelled);

    CHECK_FALSE(result.success);
    CHECK(result.error == "MVLC reported a command-stack error");
    CHECK(transport.SentRequests().size() == 6U);
    CHECK(nextSuperReference == 306U);
    CHECK(nextStackReference == 3003U);
}

TEST_CASE("an incomplete execute scan retries and retains its error")
{
    using namespace fidget;
    using namespace fidget::test;

    constexpr std::uint32_t Address = 0x11006004U;
    FakeCommandTransport transport;
    Open(transport);
    for (std::uint16_t attempt = 0U;
         attempt < MvlcTransactionAttemptCount;
         ++attempt)
    {
        const std::uint16_t uploadReference =
            static_cast<std::uint16_t>(400U + attempt * 2U);
        const std::uint16_t executeReference = uploadReference + 1U;
        const std::uint32_t stackReference = 4000U + attempt;
        transport.QueueExchange({
            MakeUploadRequest(uploadReference, stackReference, Address),
            {FakeReceiveAction::Datagram(
                MakeCommandPacket({MakeSuperFrame(uploadReference)}))},
        });

        std::vector<FakeReceiveAction> incompleteReplies;
        for (int packet = 0;
             packet < MvlcMaximumResponseDatagrams;
             ++packet)
        {
            incompleteReplies.push_back(FakeReceiveAction::Datagram(
                MakeCommandPacket({MakeSuperFrame(executeReference)})));
        }
        transport.QueueExchange({
            MakeExecuteRequest(executeReference),
            std::move(incompleteReplies),
        });
    }

    std::uint16_t nextSuperReference = 400U;
    std::uint32_t nextStackReference = 4000U;
    const std::atomic<bool> cancelled{false};
    const auto result = ReadVmeD16(
        transport,
        Address,
        nextSuperReference,
        nextStackReference,
        cancelled);

    CHECK_FALSE(result.success);
    CHECK(result.error == "Incomplete MVLC VME-read response");
    CHECK(transport.SentRequests().size() == 6U);
    CHECK(transport.ReceiveCapacities().size() == 27U);
    CHECK(nextSuperReference == 406U);
    CHECK(nextStackReference == 4003U);
}

TEST_CASE("a VME write uses the guarded stack path")
{
    using namespace fidget;
    using namespace fidget::test;

    constexpr std::uint32_t Address = 0x11006030U;
    const auto operation = EncodeMvlcVmeWriteD16Words(Address, 1U);
    const auto uploadWords = BuildMvlcStackUploadRequest(
        0x40U, 0x500U, operation.data(), operation.size());

    FakeCommandTransport transport;
    Open(transport);
    transport.QueueExchange({
        EncodeWords(uploadWords),
        {FakeReceiveAction::Datagram(
            MakeCommandPacket({MakeSuperFrame(0x40U)}))},
    });
    transport.QueueExchange({
        MakeExecuteRequest(0x41U),
        {FakeReceiveAction::Datagram(MakeCommandPacket({
            MakeSuperFrame(0x41U),
            MakeWriteStackFrame(0x500U),
        }))},
    });

    std::uint16_t nextSuperReference = 0x40U;
    std::uint32_t nextStackReference = 0x500U;
    const std::atomic<bool> cancelled{false};
    const auto result = WriteVmeD16(
        transport,
        Address,
        1U,
        nextSuperReference,
        nextStackReference,
        cancelled);

    CHECK(result.success);
    CHECK(nextSuperReference == 0x42U);
    CHECK(nextStackReference == 0x501U);
}

TEST_CASE("local-register writes consume one super reference")
{
    using namespace fidget;
    using namespace fidget::test;

    constexpr std::array writes{
        MvlcLocalRegisterWrite{0x1100U, 0x00000100U},
        MvlcLocalRegisterWrite{0x1200U, 0x00000004U},
    };
    const auto request = BuildMvlcLocalRegisterWriteRequest(
        0x700U, writes.data(), writes.size());
    REQUIRE(request.success);

    FakeCommandTransport transport;
    Open(transport);
    transport.QueueExchange({
        EncodeWords(request.words),
        {
            FakeReceiveAction::Datagram(
                MakeCommandPacket({MakeSuperFrame(0x701U)})),
            FakeReceiveAction::Datagram(
                MakeCommandPacket({MakeSuperFrame(0x700U)})),
        },
    });

    std::uint16_t nextSuperReference = 0x700U;
    const std::atomic<bool> cancelled{false};
    const auto result = WriteLocalRegisters(
        transport,
        writes.data(),
        writes.size(),
        nextSuperReference,
        cancelled);

    CHECK(result.success);
    CHECK(nextSuperReference == 0x701U);
    CHECK(transport.SentRequests().size() == 1U);
    CHECK(transport.ReceiveCapacities().size() == 2U);

    const auto empty = WriteLocalRegisters(
        transport,
        nullptr,
        0U,
        nextSuperReference,
        cancelled);
    CHECK_FALSE(empty.success);
    CHECK(empty.error == "No MVLC-local register writes were requested");
    CHECK(nextSuperReference == 0x701U);
}

TEST_CASE("cancellation stops a transaction between response datagrams")
{
    using namespace fidget;
    using namespace fidget::test;

    constexpr std::uint32_t Address = 0x11006004U;
    std::atomic<bool> cancelled{false};
    FakeCommandTransport transport;
    Open(transport);
    transport.SetReceiveHook([&cancelled](std::size_t receiveCount) {
        if (receiveCount == 1U)
        {
            cancelled.store(true);
        }
    });
    transport.QueueExchange({
        MakeUploadRequest(0x900U, 0x1000U, Address),
        {FakeReceiveAction::Datagram(
            MakeCommandPacket({MakeSuperFrame(0x7777U)}))},
    });

    std::uint16_t nextSuperReference = 0x900U;
    std::uint32_t nextStackReference = 0x1000U;
    const auto result = ReadVmeD16(
        transport,
        Address,
        nextSuperReference,
        nextStackReference,
        cancelled);

    CHECK_FALSE(result.success);
    CHECK(result.error == "MDPP inspection cancelled");
    CHECK(transport.SentRequests().size() == 1U);
    CHECK(nextSuperReference == 0x901U);
    CHECK(nextStackReference == 0x1001U);
}
