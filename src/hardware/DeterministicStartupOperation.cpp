#include "hardware/DeterministicStartupOperation.h"

#include "core/ScpConfiguration.h"
#include "core/ScpProfile.h"
#include "core/ScpTransactionPlan.h"
#include "hardware/ScpBulkApplyOperation.h"
#include "hardware/ScpCaptureOperation.h"
#include "hardware/StartupPreparationOperation.h"

#include <string>
#include <utility>

namespace fidget {
namespace {

std::string ValidateRequest(
    const DeterministicStartupRequest& request,
    const ScpStandaloneStartupPlan& plan)
{
    if (!request.confirmed)
    {
        return "Deterministic startup requires explicit confirmation of "
               "the reviewed recipe.";
    }
    if (!request.profileLoadedForTarget)
    {
        return "Deterministic startup requires a loaded profile for the "
               "selected module.";
    }
    if (!request.configurationFresh ||
        request.reviewedConfiguration.state !=
            ScpConfigurationState::Complete)
    {
        return "Deterministic startup requires a fresh comparable "
               "eight-quad snapshot.";
    }
    if (!request.startupAuditCompleteForTarget ||
        request.startupAudit.state != StartupAuditState::Complete)
    {
        return "Deterministic startup requires a completed startup audit "
               "for the selected module.";
    }
    if (request.startupAudit.baseAddress !=
            request.reviewedConfiguration.baseAddress ||
        request.startupAudit.hardwareId != Mdpp32HardwareId ||
        request.startupAudit.firmwareRevision !=
            Mdpp32ScpFirmwareRevisionFw2051)
    {
        return "The completed startup audit does not prove the reviewed "
               "MDPP-32 hardware 0x5007 with SCP FW2051 target.";
    }
    if (!plan.success)
    {
        return plan.message;
    }
    return {};
}

DeterministicStartupResult Fail(
    DeterministicStartupResult result,
    std::string message)
{
    result.state = DeterministicStartupState::Failed;
    result.message = std::move(message);
    return result;
}

} // namespace

DeterministicStartupResult RunFw2051DeterministicStartup(
    ICommandTransport& transport,
    const DeterministicStartupRequest& request,
    const std::atomic<bool>& cancellationRequested,
    const ScpCaptureOwnershipGate& ownershipGate)
{
    DeterministicStartupResult result;
    result.baseAddress = request.reviewedConfiguration.baseAddress;
    result.reviewedPlan = PlanFw2051ScpStandaloneStartup(
        request.profile, request.reviewedConfiguration);
    result.valuesCompared = result.reviewedPlan.valuesCompared;
    result.initialDifferences =
        result.reviewedPlan.configurationDifferences;
    result.startupContractDifferences =
        result.reviewedPlan.startupContractDifferences;
    result.bankedDifferences = result.reviewedPlan.bankedDifferences;

    const auto requestError = ValidateRequest(request, result.reviewedPlan);
    if (!requestError.empty())
    {
        return Fail(std::move(result), requestError);
    }
    if (!ownershipGate)
    {
        return Fail(
            std::move(result),
            "Deterministic startup has no ownership gate.");
    }

    result.state = DeterministicStartupState::PreparingReadout;
    result.message = "Running the strict module-wide readout preparation...";
    result.preparation = PrepareFw2051ModuleForStartup(
        transport,
        result.baseAddress,
        request.startupAudit,
        cancellationRequested,
        ownershipGate);
    if (result.preparation.state != StartupPreparationState::Passed)
    {
        const auto message =
            "Startup preparation failed; no banked-profile write was "
            "requested. " + result.preparation.message;
        return Fail(std::move(result), message);
    }
    result.startupPreparationPassed = true;
    result.moduleLeftStopped = result.preparation.moduleLeftStopped;

    result.state =
        DeterministicStartupState::CapturingPreparedConfiguration;
    result.message =
        "The readout contract passed. Recapturing all eight SCP banks "
        "before any profile write...";
    const auto postPreparationCapture = CaptureFw2051ScpConfiguration(
        transport,
        result.baseAddress,
        cancellationRequested,
        ownershipGate);
    result.postPreparationConfiguration = postPreparationCapture.configuration;
    if (result.postPreparationConfiguration.state !=
        ScpConfigurationState::Complete)
    {
        const auto message =
            "The post-preparation eight-bank capture failed; no banked-"
            "profile write was requested. " +
            result.postPreparationConfiguration.message;
        return Fail(std::move(result), message);
    }
    result.postPreparationCapturePassed = true;

    result.postPreparationPlan = PlanFw2051ScpProfileApplication(
        request.profile, result.postPreparationConfiguration);
    if (!result.postPreparationPlan.success)
    {
        const auto message =
            "The freshly prepared module cannot be matched to the saved "
            "profile; no banked write was requested. " +
            result.postPreparationPlan.message;
        return Fail(std::move(result), message);
    }
    result.bankedWritesPlanned =
        result.postPreparationPlan.request.steps.size();
    result.bankedApplicationNeeded = result.bankedWritesPlanned != 0U;

    if (result.bankedApplicationNeeded)
    {
        result.state = DeterministicStartupState::ApplyingBankedProfile;
        result.message =
            "Applying the freshly planned banked differences with exact "
            "readback and rollback protection...";
        result.bankedApplication = ApplyFw2051ScpProfile(
            transport,
            result.postPreparationPlan.request,
            cancellationRequested,
            ownershipGate);
        if (result.bankedApplication.state != ScpBulkApplyState::Passed)
        {
            result.moduleLeftStopped =
                result.bankedApplication.moduleLeftStopped;
            const auto message =
                "The banked-profile phase failed; its transaction "
                "performed its guarded rollback when ownership remained "
                "available. " + result.bankedApplication.message;
            return Fail(std::move(result), message);
        }
        result.bankedApplicationPassed = true;
        result.moduleLeftStopped =
            result.bankedApplication.moduleLeftStopped;
    }
    else
    {
        result.bankedApplicationPassed = true;
    }

    result.state = DeterministicStartupState::VerifyingFinalConfiguration;
    result.message =
        "Recapturing all eight banks for the final 141-value proof...";
    const auto finalCapture = CaptureFw2051ScpConfiguration(
        transport,
        result.baseAddress,
        cancellationRequested,
        ownershipGate);
    result.finalConfiguration = finalCapture.configuration;
    if (result.finalConfiguration.state != ScpConfigurationState::Complete)
    {
        const auto message =
            "The final eight-bank verification capture failed; the final "
            "proof is unavailable. " + result.finalConfiguration.message;
        return Fail(std::move(result), message);
    }

    result.finalComparison = CompareFw2051ScpConfiguration(
        request.profile, result.finalConfiguration);
    if (!result.finalComparison.comparable ||
        result.finalComparison.valuesCompared !=
            Fw2051ScpConfigurationValueCount ||
        !result.finalComparison.differences.empty())
    {
        const auto message =
            "Final verification did not match all 141 saved SCP values. " +
            result.finalComparison.message;
        return Fail(std::move(result), message);
    }

    result.finalProfileVerified = true;
    result.state = DeterministicStartupState::Passed;
    result.message =
        "Deterministic tuner startup passed: the readout contract and all "
        "141 saved SCP values are verified. Ready and stopped.";
    return result;
}

} // namespace fidget
