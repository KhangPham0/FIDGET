#include "hardware/VmeTransaction.h"

#include <array>
#include <string>
#include <utility>
#include <vector>

namespace fidget {
namespace {

enum class RetryDisposition
{
    Retry,
    Stop,
};

struct StackExecutionResult
{
    bool success = false;
    RetryDisposition retryDisposition = RetryDisposition::Retry;
    MvlcFrame stackFrame;
    std::string error;
};

bool SendWords(ICommandTransport& transport,
               const std::uint32_t* words,
               std::size_t wordCount,
               std::string& error)
{
    const auto bytes = EncodeMvlcWordsLittleEndian(words, wordCount);
    const auto sent = transport.Send(bytes.data(), bytes.size());
    if (!sent.success)
    {
        error = sent.error;
        return false;
    }
    return true;
}

bool ReceiveFrames(ICommandTransport& transport,
                   std::vector<MvlcFrame>& frames,
                   std::string& error)
{
    std::array<std::byte, MvlcCommandResponseBufferSize> packet{};
    const auto received = transport.Receive(
        packet.data(),
        packet.size(),
        MvlcCommandResponseTimeoutMilliseconds);
    if (received.status != TransportReceiveStatus::Received)
    {
        if (!received.error.empty())
        {
            error = received.error;
        }
        else if (received.status == TransportReceiveStatus::Timeout)
        {
            error = "receive: MVLC response timed out";
        }
        else
        {
            error = "receive: MVLC transport error";
        }
        return false;
    }
    if (received.bytesReceived > packet.size())
    {
        error = "receive: MVLC response exceeds buffer";
        return false;
    }

    auto parsed = ParseMvlcCommandPacket(
        packet.data(), received.bytesReceived);
    if (!parsed.success)
    {
        error = std::move(parsed.error);
        return false;
    }

    frames = std::move(parsed.frames);
    return true;
}

bool UploadStack(ICommandTransport& transport,
                 std::uint16_t superReference,
                 std::uint32_t stackReference,
                 const std::uint32_t* operationWords,
                 std::size_t operationWordCount,
                 const std::atomic<bool>& cancellationRequested,
                 std::string& error)
{
    const auto request = BuildMvlcStackUploadRequest(
        superReference,
        stackReference,
        operationWords,
        operationWordCount);
    if (!SendWords(transport, request.data(), request.size(), error))
    {
        return false;
    }

    for (int packetIndex = 0;
         packetIndex < MvlcMaximumResponseDatagrams
             && !cancellationRequested.load();
         ++packetIndex)
    {
        std::vector<MvlcFrame> frames;
        if (!ReceiveFrames(transport, frames, error))
        {
            return false;
        }

        for (const auto& frame : frames)
        {
            const auto validation = ValidateMvlcStackUploadFrame(
                frame, superReference);
            if (!validation.matches)
            {
                continue;
            }
            if (!validation.success)
            {
                error = validation.error;
                return false;
            }
            return true;
        }
    }

    error = cancellationRequested.load()
        ? "MDPP inspection cancelled"
        : "No matching MVLC stack-upload response";
    return false;
}

bool IsTerminalVmeFailure(const MvlcFrame& frame,
                          std::uint32_t stackReference)
{
    if (frame.type != MvlcStackFrameType
        || frame.words.size() < 2U
        || frame.words[1] != stackReference)
    {
        return false;
    }

    return (frame.flags & (MvlcTimeoutFlag | MvlcBusErrorFlag)) != 0U;
}

StackExecutionResult ExecuteStack(
    ICommandTransport& transport,
    std::uint16_t superReference,
    std::uint32_t stackReference,
    const std::atomic<bool>& cancellationRequested)
{
    StackExecutionResult result;
    const auto request = BuildMvlcStackExecuteRequest(superReference);
    if (!SendWords(
            transport,
            request.data(),
            request.size(),
            result.error))
    {
        return result;
    }

    bool receivedSuperFrame = false;
    bool receivedStackFrame = false;
    MvlcFrame matchingStackFrame;

    for (int packetIndex = 0;
         packetIndex < MvlcMaximumResponseDatagrams
             && !cancellationRequested.load();
         ++packetIndex)
    {
        std::vector<MvlcFrame> frames;
        if (!ReceiveFrames(transport, frames, result.error))
        {
            return result;
        }

        for (const auto& frame : frames)
        {
            const auto superValidation =
                ValidateMvlcStackExecuteSuperFrame(frame, superReference);
            if (superValidation.matches)
            {
                if (!superValidation.success)
                {
                    result.error = superValidation.error;
                    return result;
                }
                receivedSuperFrame = true;
                continue;
            }

            const auto stackValidation =
                ValidateMvlcStackExecutionFrame(frame, stackReference);
            if (!stackValidation.matches)
            {
                continue;
            }
            if (!stackValidation.success)
            {
                result.error = stackValidation.error;
                if (IsTerminalVmeFailure(frame, stackReference))
                {
                    result.retryDisposition = RetryDisposition::Stop;
                }
                return result;
            }

            matchingStackFrame = frame;
            receivedStackFrame = true;
        }

        if (receivedSuperFrame && receivedStackFrame)
        {
            result.success = true;
            result.retryDisposition = RetryDisposition::Stop;
            result.stackFrame = std::move(matchingStackFrame);
            return result;
        }
    }

    result.error = cancellationRequested.load()
        ? "MDPP inspection cancelled"
        : "Incomplete MVLC VME-read response";
    return result;
}

StackExecutionResult RunStackTransaction(
    ICommandTransport& transport,
    const std::uint32_t* operationWords,
    std::size_t operationWordCount,
    std::uint16_t& nextSuperReference,
    std::uint32_t& nextStackReference,
    const std::atomic<bool>& cancellationRequested)
{
    StackExecutionResult lastResult;

    for (int attempt = 0;
         attempt < MvlcTransactionAttemptCount
             && !cancellationRequested.load();
         ++attempt)
    {
        const std::uint32_t stackReference = nextStackReference++;
        const std::uint16_t uploadReference = nextSuperReference++;
        std::string uploadError;

        if (!UploadStack(
                transport,
                uploadReference,
                stackReference,
                operationWords,
                operationWordCount,
                cancellationRequested,
                uploadError))
        {
            lastResult.error = std::move(uploadError);
            continue;
        }

        const std::uint16_t executeReference = nextSuperReference++;
        lastResult = ExecuteStack(
            transport,
            executeReference,
            stackReference,
            cancellationRequested);
        if (lastResult.success
            || lastResult.retryDisposition == RetryDisposition::Stop)
        {
            return lastResult;
        }
    }

    if (cancellationRequested.load())
    {
        lastResult.error = "MDPP inspection cancelled";
    }
    else if (lastResult.error.empty())
    {
        lastResult.error = "MVLC VME transaction failed";
    }
    return lastResult;
}

} // namespace

MvlcVmeReadResult ReadVmeD16(
    ICommandTransport& transport,
    std::uint32_t address,
    std::uint16_t& nextSuperReference,
    std::uint32_t& nextStackReference,
    const std::atomic<bool>& cancellationRequested)
{
    const auto operationWords = EncodeMvlcVmeReadD16Words(address);
    auto transaction = RunStackTransaction(
        transport,
        operationWords.data(),
        operationWords.size(),
        nextSuperReference,
        nextStackReference,
        cancellationRequested);

    MvlcVmeReadResult result;
    result.error = std::move(transaction.error);
    if (!transaction.success)
    {
        return result;
    }
    return ParseMvlcVmeReadD16Reply(transaction.stackFrame);
}

MvlcVmeWriteResult WriteVmeD16(
    ICommandTransport& transport,
    std::uint32_t address,
    std::uint16_t value,
    std::uint16_t& nextSuperReference,
    std::uint32_t& nextStackReference,
    const std::atomic<bool>& cancellationRequested)
{
    const auto operationWords = EncodeMvlcVmeWriteD16Words(address, value);
    auto transaction = RunStackTransaction(
        transport,
        operationWords.data(),
        operationWords.size(),
        nextSuperReference,
        nextStackReference,
        cancellationRequested);

    MvlcVmeWriteResult result;
    result.error = std::move(transaction.error);
    if (!transaction.success)
    {
        return result;
    }
    return ParseMvlcVmeWriteD16Reply(transaction.stackFrame);
}

MvlcLocalWriteResult WriteLocalRegisters(
    ICommandTransport& transport,
    const MvlcLocalRegisterWrite* writes,
    std::size_t writeCount,
    std::uint16_t& nextSuperReference,
    const std::atomic<bool>& cancellationRequested)
{
    MvlcLocalWriteResult result;
    const std::uint16_t reference = nextSuperReference;
    const auto request = BuildMvlcLocalRegisterWriteRequest(
        reference, writes, writeCount);
    if (!request.success)
    {
        result.error = request.error;
        return result;
    }
    ++nextSuperReference;

    if (!SendWords(
            transport,
            request.words.data(),
            request.words.size(),
            result.error))
    {
        return result;
    }

    for (int packetIndex = 0;
         packetIndex < MvlcMaximumResponseDatagrams
             && !cancellationRequested.load();
         ++packetIndex)
    {
        std::vector<MvlcFrame> frames;
        if (!ReceiveFrames(transport, frames, result.error))
        {
            return result;
        }

        for (const auto& frame : frames)
        {
            const auto validation = ValidateMvlcLocalWriteFrame(
                frame, reference);
            if (!validation.matches)
            {
                continue;
            }
            if (!validation.success)
            {
                result.error = validation.error;
                return result;
            }

            result.success = true;
            return result;
        }
    }

    result.error = cancellationRequested.load()
        ? "MVLC local-register transaction cancelled"
        : "No matching MVLC local-register response";
    return result;
}

MvlcLocalBatchReadResult ReadLocalRegisters(
    ICommandTransport& transport,
    const std::uint16_t* addresses,
    const std::size_t addressCount,
    std::uint16_t& nextReference,
    const std::atomic<bool>& cancellationRequested)
{
    MvlcLocalBatchReadResult result;
    const std::uint16_t reference = nextReference++;
    const auto request = BuildMvlcLocalRegisterBatchReadRequest(
        reference, addresses, addressCount);
    if (!request.success)
    {
        result.error = request.error;
        return result;
    }
    const auto requestBytes = EncodeMvlcWordsLittleEndian(
        request.words.data(), request.words.size());
    std::array<std::byte, MvlcCommandResponseBufferSize> response{};

    for (int attempt = 0;
         attempt < MvlcFingerprintReadAttemptCount
             && !cancellationRequested.load();
         ++attempt)
    {
        const auto sent = transport.Send(
            requestBytes.data(), requestBytes.size());
        if (!sent.success)
        {
            result.error = sent.error;
            return result;
        }

        for (int packetIndex = 0;
             packetIndex < MvlcMaximumResponseDatagrams
                 && !cancellationRequested.load();
             ++packetIndex)
        {
            const auto received = transport.Receive(
                response.data(),
                response.size(),
                MvlcCommandResponseTimeoutMilliseconds);
            if (received.status == TransportReceiveStatus::Timeout)
            {
                break;
            }
            if (received.status == TransportReceiveStatus::Error)
            {
                result.error = received.error;
                return result;
            }

            auto parsed = ParseMvlcLocalRegisterBatchReadReply(
                response.data(),
                received.bytesReceived,
                reference,
                addresses,
                addressCount);
            if (parsed.status == MvlcLocalReadReplyStatus::Match)
            {
                result.success = true;
                result.values = std::move(parsed.values);
                return result;
            }
            if (parsed.status == MvlcLocalReadReplyStatus::Malformed)
            {
                result.error =
                    "receive: malformed MVLC register-batch response";
                return result;
            }
        }
    }

    result.error = cancellationRequested.load()
        ? "MVLC register-batch read cancelled"
        : "No matching MVLC response after six register-batch attempts";
    return result;
}

} // namespace fidget
