#include "core/TunerSnapshot.h"

namespace fidget {

GuidedTunerInputs MakeGuidedTunerInputs(
    const TunerSnapshot& snapshot) noexcept
{
    GuidedTunerInputs inputs;
    inputs.projectActive = snapshot.projectActive;
    inputs.targetSupported = snapshot.targetSupported;
    inputs.ownership = snapshot.ownership;
    inputs.mvmeHandoffConfirmed = snapshot.mvmeHandoffConfirmed;
    inputs.recoveryRecordAvailable = snapshot.recoveryRecordAvailable;
    inputs.activeOperation = snapshot.activeOperation;
    inputs.profileLoadedForTarget = snapshot.profileLoadedForTarget;
    inputs.startupAuditCompleteForTarget =
        snapshot.startupAuditCompleteForTarget;
    inputs.startupAuditReady = snapshot.startupAuditReady;
    inputs.configurationCompleteForTarget =
        snapshot.configurationCompleteForTarget;
    inputs.configurationFresh = snapshot.configurationFresh;
    inputs.profileMatchesExactly = snapshot.profileMatchesExactly;
    inputs.startupPlanAvailable = snapshot.startupPlanAvailable;
    inputs.deterministicStartupPassed = snapshot.deterministicStartupPassed;
    inputs.acquisition = snapshot.acquisition;
    inputs.cleanupVerified = snapshot.cleanupVerified;
    return inputs;
}

} // namespace fidget
