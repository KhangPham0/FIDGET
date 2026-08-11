#ifndef FIDGET_CORE_DETERMINISTIC_STARTUP_H
#define FIDGET_CORE_DETERMINISTIC_STARTUP_H

#include "core/ScpProfile.h"
#include "core/ScpTransactionPlan.h"
#include "core/ScpTransactionResult.h"
#include "core/StartupAudit.h"
#include "core/StartupPreparation.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace fidget {

enum class DeterministicStartupState
{
    NotRun,
    PreparingReadout,
    CapturingPreparedConfiguration,
    ApplyingBankedProfile,
    VerifyingFinalConfiguration,
    Passed,
    Failed,
};

struct DeterministicStartupRequest
{
    bool profileLoadedForTarget = false;
    bool configurationFresh = false;
    bool startupAuditCompleteForTarget = false;
    bool confirmed = false;
    ScpProfile profile;
    Fw2051ScpConfigurationSnapshot reviewedConfiguration;
    StartupAuditResult startupAudit;
};

struct DeterministicStartupResult
{
    DeterministicStartupState state = DeterministicStartupState::NotRun;
    std::string message =
        "No deterministic startup sequence has been requested";
    std::uint32_t baseAddress = 0U;
    std::size_t valuesCompared = 0U;
    std::size_t initialDifferences = 0U;
    std::size_t startupContractDifferences = 0U;
    std::size_t bankedDifferences = 0U;
    std::size_t bankedWritesPlanned = 0U;
    bool startupPreparationPassed = false;
    bool postPreparationCapturePassed = false;
    bool bankedApplicationNeeded = false;
    bool bankedApplicationPassed = false;
    bool finalProfileVerified = false;
    bool moduleLeftStopped = false;
    ScpStandaloneStartupPlan reviewedPlan;
    StartupPreparationResult preparation;
    Fw2051ScpConfigurationSnapshot postPreparationConfiguration;
    ScpProfileApplicationPlan postPreparationPlan;
    ScpBulkApplyResult bankedApplication;
    Fw2051ScpConfigurationSnapshot finalConfiguration;
    ScpConfigurationComparison finalComparison;
};

} // namespace fidget

#endif
