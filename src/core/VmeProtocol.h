#ifndef FIDGET_CORE_VME_PROTOCOL_H
#define FIDGET_CORE_VME_PROTOCOL_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fidget {

inline constexpr std::uint32_t MvlcCommandBufferStart = 0xF1000000U;
inline constexpr std::uint32_t MvlcCommandBufferEnd = 0xF2000000U;
inline constexpr std::uint32_t MvlcReferenceWordCommand = 0x01010000U;
inline constexpr std::uint32_t MvlcReadLocalCommand = 0x01020000U;
inline constexpr std::uint32_t MvlcWriteLocalCommand = 0x02040000U;

inline constexpr std::uint32_t MvlcStackStartCommand = 0xF3000000U;
inline constexpr std::uint32_t MvlcStackEndCommand = 0xF4000000U;
inline constexpr std::uint32_t MvlcWriteMarkerCommand = 0xC2000000U;
inline constexpr std::uint32_t MvlcVmeReadA32D16Command = 0x12090001U;
inline constexpr std::uint32_t MvlcVmeWriteA32D16Command = 0x23090001U;

inline constexpr std::uint16_t MvlcStack0TriggerRegister = 0x1100U;
inline constexpr std::uint16_t MvlcStack0OffsetRegister = 0x1200U;
inline constexpr std::uint16_t MvlcStackExecutionStatus0 = 0x1400U;
inline constexpr std::uint16_t MvlcStackExecutionStatus1 = 0x1404U;
inline constexpr std::uint16_t MvlcStackMemoryBegin = 0x2000U;
inline constexpr std::uint16_t MvlcImmediateStackOffset = 4U;
inline constexpr std::uint32_t MvlcImmediateTrigger = 1U << 8U;
inline constexpr std::uint16_t MvlcLocalAddressIncrement = 4U;

inline constexpr std::uint8_t MvlcSuperFrameType = 0xF1U;
inline constexpr std::uint8_t MvlcStackFrameType = 0xF3U;
inline constexpr std::uint8_t MvlcStackErrorFrameType = 0xF7U;
inline constexpr std::uint8_t MvlcTimeoutFlag = 1U << 0U;
inline constexpr std::uint8_t MvlcBusErrorFlag = 1U << 1U;
inline constexpr std::uint8_t MvlcSyntaxErrorFlag = 1U << 2U;

struct MvlcVmeReadResult
{
    bool success = false;
    std::uint16_t value = 0;
    std::string error;
};

struct MvlcVmeWriteResult
{
    bool success = false;
    std::string error;
};

struct MvlcLocalWriteResult
{
    bool success = false;
    std::string error;
};

struct MvlcLocalRegisterWrite
{
    std::uint16_t address = 0;
    std::uint32_t value = 0;

    friend bool operator==(const MvlcLocalRegisterWrite& left,
                           const MvlcLocalRegisterWrite& right) noexcept
    {
        return left.address == right.address && left.value == right.value;
    }
};

struct MvlcFrame
{
    std::uint8_t type = 0;
    std::uint8_t flags = 0;
    std::vector<std::uint32_t> words;
};

struct MvlcCommandPacketParseResult
{
    bool success = false;
    std::vector<MvlcFrame> frames;
    std::string error;
};

struct MvlcRequestBuildResult
{
    bool success = false;
    std::vector<std::uint32_t> words;
    std::string error;
};

enum class MvlcLocalReadReplyStatus
{
    Match,
    NoMatch,
    Malformed,
};

struct MvlcLocalReadReply
{
    MvlcLocalReadReplyStatus status =
        MvlcLocalReadReplyStatus::Malformed;
    std::uint32_t value = 0U;
};

// A frame can be unrelated to the active transaction, accepted, or rejected.
// matches distinguishes an unrelated frame from a rejected matching frame.
struct MvlcFrameValidationResult
{
    bool matches = false;
    bool success = false;
    std::string error;
};

void StoreLittleEndian32(std::byte* destination, std::uint32_t value);
[[nodiscard]] std::uint32_t LoadLittleEndian32(const std::byte* source);

[[nodiscard]] std::vector<std::byte> EncodeMvlcWordsLittleEndian(
    const std::uint32_t* words,
    std::size_t wordCount);

[[nodiscard]] std::array<std::uint32_t, 2> EncodeMvlcVmeReadD16Words(
    std::uint32_t address);
[[nodiscard]] std::array<std::uint32_t, 3> EncodeMvlcVmeWriteD16Words(
    std::uint32_t address,
    std::uint16_t value);

[[nodiscard]] std::vector<std::uint32_t> BuildMvlcStackUploadRequest(
    std::uint16_t superReference,
    std::uint32_t stackReference,
    const std::uint32_t* operationWords,
    std::size_t operationWordCount);
[[nodiscard]] std::vector<std::uint32_t> BuildMvlcStackExecuteRequest(
    std::uint16_t superReference);
[[nodiscard]] std::array<std::uint32_t, 4>
    BuildMvlcLocalRegisterReadRequest(
        std::uint16_t reference,
        std::uint16_t address);
[[nodiscard]] MvlcRequestBuildResult BuildMvlcLocalRegisterWriteRequest(
    std::uint16_t superReference,
    const MvlcLocalRegisterWrite* writes,
    std::size_t writeCount);

[[nodiscard]] MvlcCommandPacketParseResult ParseMvlcCommandPacket(
    const std::byte* packet,
    std::size_t packetSize);
[[nodiscard]] MvlcLocalReadReply ParseMvlcLocalRegisterReadReply(
    const std::byte* packet,
    std::size_t packetSize,
    std::uint16_t reference,
    std::uint16_t address);
[[nodiscard]] bool IsMatchingMvlcSuperFrame(
    const MvlcFrame& frame,
    std::uint16_t reference);

[[nodiscard]] MvlcFrameValidationResult ValidateMvlcStackUploadFrame(
    const MvlcFrame& frame,
    std::uint16_t reference);
[[nodiscard]] MvlcFrameValidationResult ValidateMvlcStackExecuteSuperFrame(
    const MvlcFrame& frame,
    std::uint16_t reference);
[[nodiscard]] MvlcFrameValidationResult ValidateMvlcStackExecutionFrame(
    const MvlcFrame& frame,
    std::uint32_t stackReference);
[[nodiscard]] MvlcFrameValidationResult ValidateMvlcLocalWriteFrame(
    const MvlcFrame& frame,
    std::uint16_t reference);

[[nodiscard]] MvlcVmeReadResult ParseMvlcVmeReadD16Reply(
    const MvlcFrame& stackFrame);
[[nodiscard]] MvlcVmeWriteResult ParseMvlcVmeWriteD16Reply(
    const MvlcFrame& stackFrame);

} // namespace fidget

#endif
