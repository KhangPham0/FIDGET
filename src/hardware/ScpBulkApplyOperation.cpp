#include "hardware/ScpBulkApplyOperation.h"

#include "core/ScpConfiguration.h"
#include "core/ScpProfile.h"
#include "core/ScpRegistry.h"
#include "hardware/VmeTransaction.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fidget {
namespace {

constexpr auto SelectorSettleTime = std::chrono::microseconds(50);
constexpr auto FrontendSettleTime = std::chrono::microseconds(20);
constexpr std::uint16_t InitialSuperReference = 0x3C00U;
constexpr std::uint32_t InitialStackReference = 0x9D200001U;

std::string RegisterOperationError(
    const char* operation,
    const std::string& name,
    const std::uint16_t registerOffset,
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

std::string ValidateRequest(const ScpProfileApplicationRequest& request)
{
    const auto configurationError =
        ValidateFw2051ScpConfiguration(request.expectedLiveConfiguration);
    if (!configurationError.empty())
    {
        return "The expected live configuration is invalid: " +
            configurationError;
    }
    if (request.steps.empty())
    {
        return "The complete SCP profile application contains no writes.";
    }

    auto target = request.expectedLiveConfiguration;
    for (const auto& step : request.steps)
    {
        if (step.quad < 0 ||
            static_cast<std::size_t>(step.quad) >= target.quads.size())
        {
            return "A complete SCP profile step has an invalid channel "
                   "quad.";
        }
        const auto* definition = FindFw2051ScpSetting(step.registerOffset);
        if (definition == nullptr)
        {
            return "Retained SCP profile application is not available for "
                   "a requested register.";
        }
        const auto expectedValue = Fw2051ScpQuadRegisterValue(
            target.quads[static_cast<std::size_t>(step.quad)],
            step.registerOffset);
        if (!expectedValue || *expectedValue != step.expectedValue)
        {
            return "A complete SCP profile step does not match its expected "
                   "live configuration.";
        }
        const auto valueError = ValidateFw2051ScpProfileValue(
            step.registerOffset, step.profileValue);
        if (!valueError.empty())
        {
            return valueError;
        }
        if (!SetFw2051ScpQuadRegisterValue(
                target.quads[static_cast<std::size_t>(step.quad)],
                step.registerOffset,
                step.profileValue))
        {
            return "The FW2051 registry does not map a requested profile "
                   "value.";
        }
    }

    const auto targetError = ValidateFw2051ScpConfiguration(target);
    if (!targetError.empty())
    {
        return "The complete SCP profile target is invalid: " + targetError;
    }

    for (std::size_t quadIndex = 0U;
         quadIndex < target.quads.size();
         ++quadIndex)
    {
        const auto& quad = target.quads[quadIndex];
        for (const auto& definition : Fw2051ScpSettingRegistry)
        {
            const auto value = Fw2051ScpQuadRegisterValue(
                quad, definition.registerOffset);
            if (!value)
            {
                return "The FW2051 registry does not map every profile "
                       "value.";
            }
            const auto valueError = ValidateFw2051ScpProfileValue(
                definition.registerOffset, *value);
            if (!valueError.empty())
            {
                return "Quad " + std::to_string(quadIndex) + ": " +
                    valueError;
            }
        }
        if (quad.timingFilter > quad.shapingTime)
        {
            return "Quad " + std::to_string(quadIndex) +
                " has a profile timing filter greater than its shaping "
                "time.";
        }
        if (quad.preSamples >= quad.totalSamples)
        {
            return "Quad " + std::to_string(quadIndex) +
                " has profile pre-samples at or after total samples.";
        }
    }
    return {};
}

std::string GateFailureMessage(
    const ScpCaptureGateResult& gate,
    const std::atomic<bool>& cancellationRequested)
{
    if (!gate.message.empty())
    {
        return gate.message;
    }
    return cancellationRequested.load()
        ? "The complete SCP profile application was cancelled."
        : "Ownership could not be verified for the complete SCP profile "
          "application.";
}

} // namespace

ScpBulkApplyResult ApplyFw2051ScpProfile(
    ICommandTransport& transport,
    const ScpProfileApplicationRequest& request,
    const std::atomic<bool>& cancellationRequested,
    const ScpCaptureOwnershipGate& ownershipGate)
{
    ScpBulkApplyResult result;
    result.state = ScpBulkApplyState::Applying;
    result.message =
        "Revalidating the target and rereading all 140 hardware values "
        "before the first profile write...";
    result.baseAddress = request.expectedLiveConfiguration.baseAddress;
    result.valuesCompared = request.valuesCompared;
    result.configurationDifferences = request.configurationDifferences;
    result.plannedWrites = request.steps.size();
    result.values.reserve(request.steps.size());
    for (const auto& step : request.steps)
    {
        result.values.push_back({
            step.quad,
            step.registerOffset,
            step.settingName,
            step.expectedValue,
            step.profileValue,
        });
    }

    std::string failure = ValidateRequest(request);
    if (!failure.empty())
    {
        result.state = ScpBulkApplyState::Failed;
        result.message = failure;
        return result;
    }
    if (!ownershipGate)
    {
        result.state = ScpBulkApplyState::Failed;
        result.message =
            "The complete SCP profile application has no ownership gate.";
        return result;
    }

    const auto capture = CaptureFw2051ScpConfiguration(
        transport,
        result.baseAddress,
        cancellationRequested,
        ownershipGate);
    result.selectorParkedAtQuadZero =
        capture.configuration.selectorParkedAtQuadZero;
    if (capture.configuration.state != ScpConfigurationState::Complete)
    {
        result.state = ScpBulkApplyState::Failed;
        result.message = capture.configuration.message;
        return result;
    }

    ScpProfile expectedProfile;
    expectedProfile.configuration = request.expectedLiveConfiguration;
    const auto comparison = CompareFw2051ScpConfiguration(
        expectedProfile, capture.configuration);
    if (!comparison.comparable || !comparison.differences.empty())
    {
        result.state = ScpBulkApplyState::Failed;
        result.message = comparison.comparable
            ? "The full SCP preflight no longer matches the captured "
              "configuration. No profile write was sent."
            : "The full SCP preflight could not be compared: " +
                comparison.message;
        return result;
    }
    result.fullPreflightMatched = true;

    std::uint16_t nextSuperReference = InitialSuperReference;
    std::uint32_t nextStackReference = InitialStackReference;
    bool ownershipAvailable = true;
    const auto appendFailure = [&failure](std::string message) {
        if (!failure.empty())
        {
            failure += ' ';
        }
        failure += std::move(message);
    };
    const auto reverifyOwnership = [&](const std::string& operationName) {
        if (!ownershipAvailable)
        {
            return false;
        }
        const auto gate = ownershipGate(operationName);
        if (gate.status == ScpCaptureGateStatus::Allowed)
        {
            return true;
        }
        ownershipAvailable = false;
        appendFailure(GateFailureMessage(gate, cancellationRequested));
        return false;
    };
    const auto readRegister = [&](
                                  const std::uint16_t registerOffset,
                                  std::uint16_t& destination,
                                  std::string& error) {
        const auto read = ReadVmeD16(
            transport,
            result.baseAddress + registerOffset,
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
            result.baseAddress + registerOffset,
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

    std::string error;
    if (reverifyOwnership("the complete SCP profile module stop"))
    {
        if (!writeRegister(
                Fw2051AcquisitionControlRegister,
                Fw2051StopAcquisitionValue,
                error))
        {
            appendFailure(RegisterOperationError(
                "write",
                "stop acquisition",
                Fw2051AcquisitionControlRegister,
                error));
        }
        else
        {
            result.moduleStopSent = true;
            std::uint16_t stopReadback = 0xFFFFU;
            if (!readRegister(
                    Fw2051AcquisitionControlRegister,
                    stopReadback,
                    error))
            {
                appendFailure(RegisterOperationError(
                    "verify",
                    "stopped acquisition",
                    Fw2051AcquisitionControlRegister,
                    error));
            }
            else if (stopReadback != Fw2051StopAcquisitionValue)
            {
                appendFailure(
                    "Acquisition-enable readback was " +
                    std::to_string(stopReadback) +
                    ", expected zero after Stop.");
            }
            else
            {
                result.moduleStopVerified = true;
                result.moduleLeftStopped = true;
            }
        }
    }

    int selectedQuad = -1;
    std::vector<std::size_t> attemptedValueIndexes;
    for (std::size_t index = 0U;
         failure.empty() && ownershipAvailable &&
         index < request.steps.size();
         ++index)
    {
        const auto& step = request.steps[index];
        auto& value = result.values[index];
        if (selectedQuad != step.quad)
        {
            if (!reverifyOwnership(
                    "complete SCP profile bank " +
                    std::to_string(step.quad)))
            {
                break;
            }
            if (!writeRegister(
                    Fw2051ScpSelectorRegister,
                    static_cast<std::uint16_t>(step.quad),
                    error))
            {
                appendFailure(RegisterOperationError(
                    "write",
                    "channel-quad selector",
                    Fw2051ScpSelectorRegister,
                    error));
                break;
            }
            selectedQuad = step.quad;
            result.selectorParkedAtQuadZero = false;
            std::this_thread::sleep_for(SelectorSettleTime);
        }

        if (!reverifyOwnership(
                "the complete SCP profile " + step.settingName +
                " write"))
        {
            break;
        }

        value.writeAttempted = true;
        ++result.writesAttempted;
        attemptedValueIndexes.push_back(index);
        if (!writeRegister(
                step.registerOffset, step.profileValue, error))
        {
            appendFailure(RegisterOperationError(
                "write", step.settingName, step.registerOffset, error));
            break;
        }

        std::this_thread::sleep_for(FrontendSettleTime);
        if (!readRegister(
                step.registerOffset, value.appliedReadback, error))
        {
            appendFailure(RegisterOperationError(
                "verify", step.settingName, step.registerOffset, error));
            break;
        }
        if (value.appliedReadback != step.profileValue)
        {
            appendFailure(
                "The applied " + step.settingName + " readback was " +
                std::to_string(value.appliedReadback) + ", expected " +
                std::to_string(step.profileValue) + '.');
            break;
        }

        value.writeVerified = true;
        value.profileValueRetained = true;
        ++result.writesVerified;
    }

    const auto parkSelector = [&]() {
        if (!ownershipAvailable ||
            !reverifyOwnership(
                "the complete SCP profile selector parking"))
        {
            return false;
        }
        std::string parkingError;
        if (!writeRegister(
                Fw2051ScpSelectorRegister, 0U, parkingError))
        {
            appendFailure(RegisterOperationError(
                "park",
                "channel-quad selector",
                Fw2051ScpSelectorRegister,
                parkingError));
            return false;
        }
        result.selectorParkedAtQuadZero = true;
        std::this_thread::sleep_for(SelectorSettleTime);
        return true;
    };
    const auto resetReadout = [&]() {
        if (!result.moduleStopVerified || !ownershipAvailable ||
            !reverifyOwnership(
                "the complete SCP profile readout reset"))
        {
            return false;
        }
        std::string resetError;
        if (!writeRegister(
                Fw2051FifoResetRegister,
                Fw2051ResetCommandValue,
                resetError))
        {
            appendFailure(RegisterOperationError(
                "write", "reset FIFO", Fw2051FifoResetRegister,
                resetError));
            return false;
        }
        result.fifoResetSent = true;
        if (!writeRegister(
                Fw2051ReadoutResetRegister,
                Fw2051ResetCommandValue,
                resetError))
        {
            appendFailure(RegisterOperationError(
                "write", "reset readout", Fw2051ReadoutResetRegister,
                resetError));
            return false;
        }
        result.readoutResetSent = true;
        return true;
    };

    if (failure.empty() && result.moduleStopVerified &&
        result.writesVerified == result.plannedWrites)
    {
        result.profileValuesRetained = true;
        parkSelector();
        if (failure.empty())
        {
            resetReadout();
        }
    }

    if (!failure.empty() && ownershipAvailable &&
        !attemptedValueIndexes.empty())
    {
        result.state = ScpBulkApplyState::RollingBack;
        result.rollbackAttempted = true;
        result.profileValuesRetained = false;
        selectedQuad = -1;
        bool rollbackSafe = ownershipAvailable;
        for (auto attempted = attemptedValueIndexes.rbegin();
             attempted != attemptedValueIndexes.rend();
             ++attempted)
        {
            auto& value = result.values[*attempted];
            if (!ownershipAvailable)
            {
                rollbackSafe = false;
                break;
            }
            if (selectedQuad != value.quad)
            {
                if (!reverifyOwnership(
                        "complete SCP profile rollback bank " +
                        std::to_string(value.quad)))
                {
                    rollbackSafe = false;
                    break;
                }
                if (!writeRegister(
                        Fw2051ScpSelectorRegister,
                        static_cast<std::uint16_t>(value.quad),
                        error))
                {
                    appendFailure(RegisterOperationError(
                        "write",
                        "rollback channel-quad selector",
                        Fw2051ScpSelectorRegister,
                        error));
                    rollbackSafe = false;
                    break;
                }
                selectedQuad = value.quad;
                std::this_thread::sleep_for(SelectorSettleTime);
            }

            if (!reverifyOwnership(
                    "the complete SCP profile " + value.settingName +
                    " rollback"))
            {
                rollbackSafe = false;
                break;
            }

            value.rollbackAttempted = true;
            ++result.rollbackWritesAttempted;
            if (!writeRegister(
                    value.registerOffset, value.expectedValue, error))
            {
                appendFailure(RegisterOperationError(
                    "rollback",
                    value.settingName,
                    value.registerOffset,
                    error));
                rollbackSafe = false;
                continue;
            }
            std::this_thread::sleep_for(FrontendSettleTime);
            if (!readRegister(
                    value.registerOffset,
                    value.rollbackReadback,
                    error))
            {
                appendFailure(RegisterOperationError(
                    "verify rollback",
                    value.settingName,
                    value.registerOffset,
                    error));
                rollbackSafe = false;
                continue;
            }
            if (value.rollbackReadback != value.expectedValue)
            {
                appendFailure(
                    "The rollback " + value.settingName +
                    " readback was " +
                    std::to_string(value.rollbackReadback) +
                    ", expected " +
                    std::to_string(value.expectedValue) + '.');
                rollbackSafe = false;
                continue;
            }
            value.rollbackVerified = true;
            value.profileValueRetained = false;
            ++result.rollbackWritesVerified;
        }

        result.rollbackVerified = rollbackSafe &&
            result.rollbackWritesVerified == attemptedValueIndexes.size();
        if (ownershipAvailable)
        {
            parkSelector();
            if (result.rollbackVerified)
            {
                resetReadout();
            }
        }
    }

    const bool passed = failure.empty() && result.fullPreflightMatched &&
        result.moduleStopSent && result.moduleStopVerified &&
        result.writesVerified == result.plannedWrites &&
        result.profileValuesRetained && result.selectorParkedAtQuadZero &&
        result.fifoResetSent && result.readoutResetSent;
    result.state = passed
        ? ScpBulkApplyState::Passed
        : ScpBulkApplyState::Failed;
    result.message = passed
        ? "Applied and retained all " +
          std::to_string(result.plannedWrites) +
          " changed banked SCP values with exact readback. The module "
          "remains stopped; recapture all eight quads before starting "
          "diagnostic acquisition."
        : result.rollbackVerified
            ? failure +
                " Every attempted profile write was restored with exact "
                "readback; the module remains stopped."
            : failure.empty()
                ? "The complete banked-profile transaction did not finish "
                  "in a verified safe state."
                : failure;
    return result;
}

} // namespace fidget
