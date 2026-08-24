#include "hardware/ScpCaptureOperation.h"

#include "core/ScpRegistry.h"
#include "hardware/VmeTransaction.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace fidget {
namespace {

constexpr std::uint16_t HardwareIdRegister = 0x6008U;
constexpr std::uint16_t FirmwareRevisionRegister = 0x600EU;
constexpr std::uint16_t IrqLevelRegister = 0x6010U;
constexpr std::uint16_t OutputFormatRegister = 0x6044U;
constexpr auto SelectorSettleTime = std::chrono::microseconds(50);
constexpr std::uint16_t InitialSuperReference = 0x1800U;
constexpr std::uint32_t InitialStackReference = 0x9C100001U;

std::string RegisterOperationError(
    const char* operation,
    const char* name,
    std::uint16_t registerOffset,
    const std::string& error)
{
    char registerText[16]{};
    std::snprintf(
        registerText,
        sizeof(registerText),
        "%04X",
        static_cast<unsigned>(registerOffset));
    return std::string("Could not ") + operation + ' ' + name +
        " at register 0x" + registerText + ": " + error;
}

std::string GateFailureMessage(
    const ScpCaptureGateResult& gate,
    const char* fallback)
{
    return gate.message.empty() ? fallback : gate.message;
}

} // namespace

ScpCaptureOperationResult CaptureFw2051ScpConfiguration(
    ICommandTransport& transport,
    const std::uint32_t baseAddress,
    const std::atomic<bool>& cancellationRequested,
    const ScpCaptureOwnershipGate& ownershipGate)
{
    return CaptureFw2051ScpConfiguration(
        transport,
        baseAddress,
        cancellationRequested,
        ownershipGate,
        {});
}

ScpCaptureOperationResult CaptureFw2051ScpConfiguration(
    ICommandTransport& transport,
    const std::uint32_t baseAddress,
    const std::atomic<bool>& cancellationRequested,
    const ScpCaptureOwnershipGate& ownershipGate,
    const ScpCaptureRuntime& runtime)
{
    ScpCaptureOperationResult operation;
    auto& result = operation.configuration;
    result.state = ScpConfigurationState::Reading;
    result.baseAddress = baseAddress;

    if ((baseAddress & 0xFFFFU) != 0U)
    {
        result.state = ScpConfigurationState::Failed;
        result.message =
            "The MDPP base must be a full 64-KiB-aligned A32 address.";
        return operation;
    }
    if (!ownershipGate)
    {
        result.state = ScpConfigurationState::Failed;
        result.message = "The SCP capture has no ownership gate.";
        operation.lastGateStatus =
            ScpCaptureGateStatus::CommunicationUnavailable;
        return operation;
    }

    const auto delay = [&runtime](const std::chrono::microseconds duration) {
        if (runtime.delay)
            runtime.delay(duration);
        else
            std::this_thread::sleep_for(duration);
    };
    const auto checkpoint = [&runtime](const ScpCaptureCheckpoint& event) {
        return !runtime.checkpoint || runtime.checkpoint(event);
    };

    const auto initialGate = ownershipGate(
        "the SCP configuration snapshot");
    operation.lastGateStatus = initialGate.status;
    if (initialGate.status != ScpCaptureGateStatus::Allowed)
    {
        result.state = ScpConfigurationState::Failed;
        result.message = GateFailureMessage(
            initialGate, "SCP configuration capture was cancelled.");
        return operation;
    }

    std::uint16_t nextSuperReference = InitialSuperReference;
    std::uint32_t nextStackReference = InitialStackReference;
    const auto readRegister = [&](
                                  const std::uint16_t registerOffset,
                                  std::uint16_t& destination,
                                  std::string& error) {
        const auto read = ReadVmeD16(
            transport,
            baseAddress + registerOffset,
            nextSuperReference,
            nextStackReference,
            cancellationRequested);
        if (!read.success)
        {
            error = read.error;
            return false;
        }
        destination = read.value;
        return true;
    };
    const auto writeRegister = [&](
                                   const std::uint16_t registerOffset,
                                   const std::uint16_t value,
                                   std::string& error) {
        const auto write = WriteVmeD16(
            transport,
            baseAddress + registerOffset,
            value,
            nextSuperReference,
            nextStackReference,
            cancellationRequested);
        if (!write.success)
        {
            error = write.error;
            return false;
        }
        return true;
    };
    const auto writeSelector = [&](
                                   const std::uint16_t value,
                                   const bool parkingWrite,
                                   std::string& error) {
        const bool success = writeRegister(
            Fw2051ScpSelectorRegister, value, error);
        result.selectorWrites.push_back({
            Fw2051ScpSelectorRegister,
            value,
            parkingWrite,
            success,
            success ? "Selector write completed." : error,
        });
        return success;
    };

    struct GlobalRead
    {
        std::uint16_t registerOffset;
        const char* name;
        std::uint16_t* destination;
    };

    const GlobalRead globalReads[] = {
        {HardwareIdRegister, "hardware ID", &result.hardwareId},
        {FirmwareRevisionRegister,
         "firmware revision",
         &result.firmwareRevision},
        {IrqLevelRegister, "IRQ level", &result.irqLevel},
        {OutputFormatRegister, "output format", &result.outputFormat},
    };

    std::string failure;
    bool selectionAttempted = false;
    bool ownershipCertain = true;
    for (const auto& read : globalReads)
    {
        std::string error;
        if (!readRegister(read.registerOffset, *read.destination, error))
        {
            failure = RegisterOperationError(
                "read", read.name, read.registerOffset, error);
            break;
        }
        if (!checkpoint({
                ScpCaptureCheckpointKind::GlobalRegisterRead,
                std::nullopt,
                read.registerOffset,
            }))
        {
            failure = "SCP configuration capture was interrupted after a "
                      "global register read.";
            ownershipCertain = false;
            operation.lastGateStatus = ScpCaptureGateStatus::Cancelled;
            break;
        }
    }

    if (failure.empty()
        && !IsWriteApprovedMdpp32HardwareId(result.hardwareId))
    {
        failure = result.hardwareId == Mdpp32AlternateHardwareId
            ? "MDPP-32 v2 selector-write support awaits recorded hardware "
              "acceptance."
            : "The selected VME base is not a write-approved MDPP-32.";
    }
    if (failure.empty() &&
        result.firmwareRevision != Mdpp32ScpFirmwareRevisionFw2051)
    {
        failure =
            "The selected MDPP does not report supported SCP FW2051 "
            "firmware; this register map cannot be applied safely.";
    }

    result.quads.reserve(Fw2051ScpQuadCount);

    for (std::uint16_t quadIndex = 0U;
         failure.empty() && quadIndex < Fw2051ScpQuadCount;
         ++quadIndex)
    {
        if (quadIndex > 0U || runtime.gateBeforeFirstSelector)
        {
            const auto gate = ownershipGate(
                "SCP configuration bank " + std::to_string(quadIndex));
            operation.lastGateStatus = gate.status;
            if (gate.status != ScpCaptureGateStatus::Allowed)
            {
                ownershipCertain = false;
                failure = GateFailureMessage(
                    gate, "SCP configuration capture was cancelled.");
                break;
            }
        }

        Fw2051ScpQuadConfiguration quad;
        quad.quad = quadIndex;
        std::string selectorError;
        selectionAttempted = true;
        if (!writeSelector(quadIndex, false, selectorError))
        {
            failure = RegisterOperationError(
                "write",
                "channel-quad selector",
                Fw2051ScpSelectorRegister,
                selectorError);
            break;
        }
        delay(SelectorSettleTime);
        if (!checkpoint({
                ScpCaptureCheckpointKind::SelectorSettled,
                quadIndex,
                Fw2051ScpSelectorRegister,
            }))
        {
            failure = "SCP configuration capture was interrupted after "
                      "selecting quad " + std::to_string(quadIndex) + '.';
            ownershipCertain = false;
            operation.lastGateStatus = ScpCaptureGateStatus::Cancelled;
            break;
        }

        for (const auto& definition : Fw2051ScpSettingRegistry)
        {
            std::uint16_t value = 0U;
            std::string readError;
            if (!readRegister(
                    definition.registerOffset, value, readError))
            {
                failure = "Quad " + std::to_string(quadIndex) + ": " +
                    RegisterOperationError(
                        "read",
                        definition.name,
                        definition.registerOffset,
                        readError);
                break;
            }
            if (!SetFw2051ScpQuadRegisterValue(
                    quad, definition.registerOffset, value))
            {
                failure =
                    "The FW2051 registry does not map every captured value.";
                break;
            }
            if (!checkpoint({
                    ScpCaptureCheckpointKind::BankedRegisterRead,
                    quadIndex,
                    definition.registerOffset,
                }))
            {
                failure = "SCP configuration capture was interrupted after "
                          "a banked register read in quad "
                    + std::to_string(quadIndex) + '.';
                ownershipCertain = false;
                operation.lastGateStatus = ScpCaptureGateStatus::Cancelled;
                break;
            }
        }

        if (failure.empty())
        {
            result.quads.push_back(std::move(quad));
        }
    }

    if (selectionAttempted && ownershipCertain)
    {
        const auto parkingGate = ownershipGate(
            "the SCP selector parking write");
        operation.lastGateStatus = parkingGate.status;
        if (parkingGate.status != ScpCaptureGateStatus::Allowed)
        {
            ownershipCertain = false;
            failure = GateFailureMessage(
                parkingGate, "SCP configuration capture was cancelled.");
        }
    }

    if (selectionAttempted && ownershipCertain)
    {
        std::string parkingError;
        result.selectorParkedAtQuadZero = writeSelector(
            0U, true, parkingError);
        if (result.selectorParkedAtQuadZero)
        {
            delay(SelectorSettleTime);
            if (!checkpoint({
                    ScpCaptureCheckpointKind::SelectorParked,
                    0U,
                    Fw2051ScpSelectorRegister,
                }))
            {
                ownershipCertain = false;
                operation.lastGateStatus = ScpCaptureGateStatus::Cancelled;
                if (!failure.empty())
                    failure += ' ';
                failure += "SCP configuration capture was interrupted after "
                           "parking the selector.";
            }
        }
        else
        {
            if (!failure.empty())
            {
                failure += ' ';
            }
            failure += "Could not park the selector at quad 0: " +
                parkingError;
        }
    }

    const bool complete = failure.empty() &&
        result.quads.size() == Fw2051ScpQuadCount &&
        result.selectorParkedAtQuadZero;
    result.state = complete
        ? ScpConfigurationState::Complete
        : ScpConfigurationState::Failed;
    result.message = complete
        ? "Captured all eight SCP channel quads. No filtering or waveform "
          "parameter was changed; the selector is parked at quad 0."
        : failure.empty()
            ? "The SCP configuration snapshot was incomplete."
            : failure;
    return operation;
}

} // namespace fidget
