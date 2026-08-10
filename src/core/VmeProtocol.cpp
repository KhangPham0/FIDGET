#include "core/VmeProtocol.h"

#include <cstddef>
#include <utility>

namespace fidget {
namespace {

constexpr std::size_t EthernetHeaderWords = 2U;
constexpr std::uint32_t MaximumCommandPipePacketChannel = 1U;

void AppendWriteLocal(std::vector<std::uint32_t>& request,
                      std::uint16_t address,
                      std::uint32_t value)
{
    request.push_back(MvlcWriteLocalCommand | address);
    request.push_back(value);
}

MvlcFrameValidationResult ValidateMatchingSuperFrame(
    const MvlcFrame& frame,
    std::uint16_t reference,
    const char* syntaxError)
{
    MvlcFrameValidationResult result;
    if (!IsMatchingMvlcSuperFrame(frame, reference))
    {
        return result;
    }

    result.matches = true;
    if ((frame.flags & MvlcSyntaxErrorFlag) != 0U)
    {
        result.error = syntaxError;
        return result;
    }

    result.success = true;
    return result;
}

} // namespace

void StoreLittleEndian32(std::byte* destination, std::uint32_t value)
{
    destination[0] = static_cast<std::byte>(value & 0xFFU);
    destination[1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    destination[2] = static_cast<std::byte>((value >> 16U) & 0xFFU);
    destination[3] = static_cast<std::byte>((value >> 24U) & 0xFFU);
}

std::uint32_t LoadLittleEndian32(const std::byte* source)
{
    return std::to_integer<std::uint32_t>(source[0])
        | (std::to_integer<std::uint32_t>(source[1]) << 8U)
        | (std::to_integer<std::uint32_t>(source[2]) << 16U)
        | (std::to_integer<std::uint32_t>(source[3]) << 24U);
}

std::vector<std::byte> EncodeMvlcWordsLittleEndian(
    const std::uint32_t* words,
    std::size_t wordCount)
{
    std::vector<std::byte> bytes(wordCount * sizeof(std::uint32_t));

    for (std::size_t index = 0; index < wordCount; ++index)
    {
        StoreLittleEndian32(bytes.data() + index * sizeof(std::uint32_t),
                            words[index]);
    }

    return bytes;
}

std::array<std::uint32_t, 2> EncodeMvlcVmeReadD16Words(
    std::uint32_t address)
{
    return {MvlcVmeReadA32D16Command, address};
}

std::array<std::uint32_t, 3> EncodeMvlcVmeWriteD16Words(
    std::uint32_t address,
    std::uint16_t value)
{
    return {MvlcVmeWriteA32D16Command,
            address,
            static_cast<std::uint32_t>(value)};
}

std::vector<std::uint32_t> BuildMvlcStackUploadRequest(
    std::uint16_t superReference,
    std::uint32_t stackReference,
    const std::uint32_t* operationWords,
    std::size_t operationWordCount)
{
    std::vector<std::uint32_t> request{
        MvlcCommandBufferStart,
        MvlcReferenceWordCommand | superReference,
    };

    std::uint16_t writeAddress =
        MvlcStackMemoryBegin + MvlcImmediateStackOffset;
    AppendWriteLocal(request, writeAddress, MvlcStackStartCommand);
    writeAddress += MvlcLocalAddressIncrement;
    AppendWriteLocal(request, writeAddress, MvlcWriteMarkerCommand);
    writeAddress += MvlcLocalAddressIncrement;
    AppendWriteLocal(request, writeAddress, stackReference);
    writeAddress += MvlcLocalAddressIncrement;

    for (std::size_t index = 0; index < operationWordCount; ++index)
    {
        AppendWriteLocal(request, writeAddress, operationWords[index]);
        writeAddress += MvlcLocalAddressIncrement;
    }

    AppendWriteLocal(request, writeAddress, MvlcStackEndCommand);
    request.push_back(MvlcCommandBufferEnd);
    return request;
}

std::vector<std::uint32_t> BuildMvlcStackExecuteRequest(
    std::uint16_t superReference)
{
    std::vector<std::uint32_t> request{
        MvlcCommandBufferStart,
        MvlcReferenceWordCommand | superReference,
    };
    AppendWriteLocal(request, MvlcStackExecutionStatus0, 0U);
    AppendWriteLocal(request, MvlcStackExecutionStatus1, 0U);
    AppendWriteLocal(request,
                     MvlcStack0OffsetRegister,
                     MvlcImmediateStackOffset);
    AppendWriteLocal(request,
                     MvlcStack0TriggerRegister,
                     MvlcImmediateTrigger);
    request.push_back(MvlcCommandBufferEnd);
    return request;
}

std::array<std::uint32_t, 4> BuildMvlcLocalRegisterReadRequest(
    std::uint16_t reference,
    std::uint16_t address)
{
    return {
        MvlcCommandBufferStart,
        MvlcReferenceWordCommand | reference,
        MvlcReadLocalCommand | address,
        MvlcCommandBufferEnd,
    };
}

MvlcRequestBuildResult BuildMvlcLocalRegisterWriteRequest(
    std::uint16_t superReference,
    const MvlcLocalRegisterWrite* writes,
    std::size_t writeCount)
{
    MvlcRequestBuildResult result;
    if (writeCount == 0)
    {
        result.error = "No MVLC-local register writes were requested";
        return result;
    }

    result.words = {
        MvlcCommandBufferStart,
        MvlcReferenceWordCommand | superReference,
    };
    result.words.reserve(3U + writeCount * 2U);

    for (std::size_t index = 0; index < writeCount; ++index)
    {
        AppendWriteLocal(result.words, writes[index].address, writes[index].value);
    }

    result.words.push_back(MvlcCommandBufferEnd);
    result.success = true;
    return result;
}

MvlcCommandPacketParseResult ParseMvlcCommandPacket(
    const std::byte* packet,
    std::size_t packetSize)
{
    MvlcCommandPacketParseResult result;

    if (packetSize < (EthernetHeaderWords + 1U) * sizeof(std::uint32_t)
        || packetSize % sizeof(std::uint32_t) != 0U)
    {
        result.error = "receive: malformed MVLC Ethernet packet";
        return result;
    }

    const std::uint32_t header0 = LoadLittleEndian32(packet);
    const std::uint32_t magic = (header0 >> 30U) & 0x3U;
    const std::uint32_t packetChannel = (header0 >> 28U) & 0x3U;
    const std::size_t payloadWords = header0 & 0x1FFFU;
    const std::size_t availableWords =
        packetSize / sizeof(std::uint32_t) - EthernetHeaderWords;

    // The MVLC command UDP pipe carries two packet channels: channel 0 for
    // low-level command mirrors and channel 1 for immediate-stack output.
    if (magic != 0U
        || packetChannel > MaximumCommandPipePacketChannel
        || payloadWords > availableWords)
    {
        result.error = "receive: invalid MVLC command-pipe header";
        return result;
    }

    const std::byte* payload =
        packet + EthernetHeaderWords * sizeof(std::uint32_t);
    std::size_t offset = 0U;

    while (offset < payloadWords)
    {
        const std::uint32_t frameHeader =
            LoadLittleEndian32(payload + offset * sizeof(std::uint32_t));
        const std::size_t frameContents = frameHeader & 0x1FFFU;
        if (offset + 1U + frameContents > payloadWords)
        {
            result.error = "receive: truncated MVLC response frame";
            return result;
        }

        MvlcFrame frame;
        frame.type = static_cast<std::uint8_t>(frameHeader >> 24U);
        frame.flags =
            static_cast<std::uint8_t>((frameHeader >> 20U) & 0x0FU);
        frame.words.reserve(frameContents + 1U);

        for (std::size_t index = 0; index <= frameContents; ++index)
        {
            frame.words.push_back(LoadLittleEndian32(
                payload + (offset + index) * sizeof(std::uint32_t)));
        }

        result.frames.push_back(std::move(frame));
        offset += frameContents + 1U;
    }

    result.success = true;
    return result;
}

MvlcLocalReadReply ParseMvlcLocalRegisterReadReply(
    const std::byte* packet,
    std::size_t packetSize,
    std::uint16_t reference,
    std::uint16_t address)
{
    MvlcLocalReadReply result;
    if (packetSize < (EthernetHeaderWords + 4U) * sizeof(std::uint32_t)
        || packetSize % sizeof(std::uint32_t) != 0U)
    {
        return result;
    }

    const std::uint32_t header0 = LoadLittleEndian32(packet);
    const std::uint32_t magic = (header0 >> 30U) & 0x3U;
    const std::uint32_t packetChannel = (header0 >> 28U) & 0x3U;
    const std::size_t payloadWords = header0 & 0x1FFFU;
    const std::size_t availableWords =
        packetSize / sizeof(std::uint32_t) - EthernetHeaderWords;
    if (magic != 0U || packetChannel != 0U
        || payloadWords > availableWords)
    {
        return result;
    }

    const std::byte* payload =
        packet + EthernetHeaderWords * sizeof(std::uint32_t);
    const std::uint32_t expectedReference =
        MvlcReferenceWordCommand | reference;
    const std::uint32_t expectedRead = MvlcReadLocalCommand | address;

    for (std::size_t offset = 0U; offset + 3U < payloadWords; ++offset)
    {
        const std::uint32_t frameHeader = LoadLittleEndian32(
            payload + offset * sizeof(std::uint32_t));
        if ((frameHeader >> 24U) != MvlcSuperFrameType)
        {
            continue;
        }

        const std::size_t frameContents = frameHeader & 0x1FFFU;
        if (frameContents < 3U
            || offset + 1U + frameContents > payloadWords)
        {
            return result;
        }

        const std::uint32_t mirroredReference = LoadLittleEndian32(
            payload + (offset + 1U) * sizeof(std::uint32_t));
        const std::uint32_t mirroredRead = LoadLittleEndian32(
            payload + (offset + 2U) * sizeof(std::uint32_t));
        if (mirroredReference != expectedReference
            || mirroredRead != expectedRead)
        {
            continue;
        }

        result.status = MvlcLocalReadReplyStatus::Match;
        result.value = LoadLittleEndian32(
            payload + (offset + 3U) * sizeof(std::uint32_t));
        return result;
    }

    result.status = MvlcLocalReadReplyStatus::NoMatch;
    return result;
}

bool IsMatchingMvlcSuperFrame(const MvlcFrame& frame,
                              std::uint16_t reference)
{
    return frame.type == MvlcSuperFrameType
        && frame.words.size() >= 2U
        && frame.words[1] == (MvlcReferenceWordCommand | reference);
}

MvlcFrameValidationResult ValidateMvlcStackUploadFrame(
    const MvlcFrame& frame,
    std::uint16_t reference)
{
    return ValidateMatchingSuperFrame(
        frame,
        reference,
        "MVLC rejected the transient VME stack");
}

MvlcFrameValidationResult ValidateMvlcStackExecuteSuperFrame(
    const MvlcFrame& frame,
    std::uint16_t reference)
{
    return ValidateMatchingSuperFrame(
        frame,
        reference,
        "MVLC rejected the immediate-stack trigger");
}

MvlcFrameValidationResult ValidateMvlcStackExecutionFrame(
    const MvlcFrame& frame,
    std::uint32_t stackReference)
{
    MvlcFrameValidationResult result;

    if (frame.type == MvlcStackErrorFrameType)
    {
        result.matches = true;
        result.error = "MVLC reported a command-stack error";
        return result;
    }

    if (frame.type != MvlcStackFrameType
        || frame.words.size() < 2U
        || frame.words[1] != stackReference)
    {
        return result;
    }

    result.matches = true;
    if ((frame.flags & MvlcTimeoutFlag) != 0U)
    {
        result.error = "VME device did not respond";
        return result;
    }
    if ((frame.flags & MvlcBusErrorFlag) != 0U)
    {
        result.error = "VME bus error";
        return result;
    }
    if ((frame.flags & MvlcSyntaxErrorFlag) != 0U)
    {
        result.error = "MVLC command-stack syntax error";
        return result;
    }

    result.success = true;
    return result;
}

MvlcFrameValidationResult ValidateMvlcLocalWriteFrame(
    const MvlcFrame& frame,
    std::uint16_t reference)
{
    return ValidateMatchingSuperFrame(
        frame,
        reference,
        "MVLC rejected the local-register transaction");
}

MvlcVmeReadResult ParseMvlcVmeReadD16Reply(const MvlcFrame& stackFrame)
{
    MvlcVmeReadResult result;
    if (stackFrame.words.size() != 3U)
    {
        result.error = "Unexpected VME-read response size";
        return result;
    }

    result.success = true;
    result.value = static_cast<std::uint16_t>(
        stackFrame.words[2] & 0xFFFFU);
    return result;
}

MvlcVmeWriteResult ParseMvlcVmeWriteD16Reply(const MvlcFrame& stackFrame)
{
    MvlcVmeWriteResult result;
    if (stackFrame.words.size() != 2U)
    {
        result.error = "Unexpected VME-write response size";
        return result;
    }

    result.success = true;
    return result;
}

} // namespace fidget
