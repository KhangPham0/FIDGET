#include "hardware/DiagnosticPauseResume.h"

#include "core/VmeProtocol.h"
#include "hardware/DiagnosticAcquisitionOperation.h"
#include "hardware/VmeTransaction.h"

#include <array>
#include <cstdio>
#include <string>
#include <utility>

namespace fidget {
namespace {

std::string RegisterOperationError(
    const char* operation,
    const char* name,
    const std::uint16_t registerOffset,
    const std::string& error)
{
    char registerText[16]{};
    std::snprintf(
        registerText,
        sizeof(registerText),
        "%04X",
        static_cast<unsigned>(registerOffset));
    return std::string("Could not ") + operation + ' ' + name
        + " at register 0x" + registerText + ": " + error;
}

void AppendError(std::string& destination, std::string message)
{
    if (!destination.empty())
    {
        destination += ' ';
    }
    destination += std::move(message);
}

} // namespace

DiagnosticPauseResult PauseDiagnosticDataTaking(
    ICommandTransport& transport,
    const std::uint32_t baseAddress,
    std::uint16_t& nextSuperReference,
    std::uint32_t& nextStackReference,
    const std::atomic<bool>& cancellationRequested)
{
    DiagnosticPauseResult result;
    const auto stopped = WriteVmeD16(
        transport,
        baseAddress + DiagnosticAcquisitionControlRegister,
        0U,
        nextSuperReference,
        nextStackReference,
        cancellationRequested);
    result.modulePaused = stopped.success;
    if (!stopped.success)
    {
        AppendError(
            result.error,
            RegisterOperationError(
                "write",
                "stop acquisition",
                DiagnosticAcquisitionControlRegister,
                stopped.error));
    }

    const MvlcLocalRegisterWrite pauseDaq{
        DiagnosticDaqModeRegister,
        0U,
    };
    const auto paused = WriteLocalRegisters(
        transport,
        &pauseDaq,
        1U,
        nextSuperReference,
        cancellationRequested);
    if (!paused.success)
    {
        AppendError(
            result.error,
            "Could not pause MVLC DAQ mode: " + paused.error);
        return result;
    }

    const std::uint16_t address = DiagnosticDaqModeRegister;
    const auto readback = ReadLocalRegisters(
        transport,
        &address,
        1U,
        nextSuperReference,
        cancellationRequested);
    result.daqModePaused = readback.success
        && !readback.values.empty()
        && readback.values.front() == 0U;
    if (!result.daqModePaused)
    {
        AppendError(
            result.error,
            readback.success
                ? "MVLC DAQ mode did not read back as zero."
                : "Could not verify paused MVLC DAQ mode: "
                    + readback.error);
    }
    return result;
}

DiagnosticResumeResult ResumeDiagnosticDataTaking(
    ICommandTransport& transport,
    const std::uint32_t baseAddress,
    std::uint16_t& nextSuperReference,
    std::uint32_t& nextStackReference,
    const std::atomic<bool>& cancellationRequested)
{
    DiagnosticResumeResult result;
    struct ResumeWrite
    {
        std::uint16_t registerOffset;
        std::uint16_t value;
        bool* outcome;
    };
    const std::array<ResumeWrite, 3> sequence{{
        {DiagnosticFifoResetRegister, 1U, &result.fifoResetSent},
        {DiagnosticReadoutResetRegister, 1U, &result.readoutResetSent},
        {DiagnosticAcquisitionControlRegister, 1U,
         &result.acquisitionResumed},
    }};
    for (const auto& write : sequence)
    {
        const auto written = WriteVmeD16(
            transport,
            baseAddress + write.registerOffset,
            write.value,
            nextSuperReference,
            nextStackReference,
            cancellationRequested);
        *write.outcome = written.success;
        if (!written.success)
        {
            AppendError(
                result.error,
                RegisterOperationError(
                    "write",
                    "resume acquisition",
                    write.registerOffset,
                    written.error));
            return result;
        }
    }

    const MvlcLocalRegisterWrite resumeDaq{
        DiagnosticDaqModeRegister,
        DiagnosticDaqEnableValue,
    };
    const auto resumed = WriteLocalRegisters(
        transport,
        &resumeDaq,
        1U,
        nextSuperReference,
        cancellationRequested);
    if (!resumed.success)
    {
        AppendError(
            result.error,
            "Could not resume MVLC DAQ mode: " + resumed.error);
        return result;
    }

    const std::uint16_t address = DiagnosticDaqModeRegister;
    const auto readback = ReadLocalRegisters(
        transport,
        &address,
        1U,
        nextSuperReference,
        cancellationRequested);
    result.daqModeReadbackValid = readback.success
        && !readback.values.empty();
    if (result.daqModeReadbackValid)
    {
        result.daqModeReadback = readback.values.front();
        result.daqModeResumed =
            (result.daqModeReadback & 0x1U) != 0U;
    }
    if (!result.daqModeResumed)
    {
        if (!result.daqModeReadbackValid)
        {
            AppendError(
                result.error,
                "Could not verify resumed MVLC DAQ mode: "
                    + readback.error);
        }
        else
        {
            char readbackText[16]{};
            std::snprintf(
                readbackText,
                sizeof(readbackText),
                "0x%08X",
                static_cast<unsigned>(result.daqModeReadback));
            AppendError(
                result.error,
                std::string("MVLC DAQ mode readback ") + readbackText
                    + " did not have enable bit 0 set.");
        }
    }
    return result;
}

} // namespace fidget
