#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/TunerControl.h"
#include "core/TunerSnapshot.h"

#include <variant>

TEST_CASE("a tuner snapshot supplies every guided workflow input")
{
    using namespace fidget;

    TunerSnapshot snapshot;
    snapshot.projectActive = true;
    snapshot.targetSupported = true;
    snapshot.ownership = GuidedTunerOwnershipState::SessionOpen;
    snapshot.mvmeHandoffConfirmed = true;
    snapshot.recoveryRecordAvailable = true;
    snapshot.activeOperation = GuidedTunerOperation::StartupPreparation;
    snapshot.profileLoadedForTarget = true;
    snapshot.startupAuditCompleteForTarget = true;
    snapshot.startupAuditReady = true;
    snapshot.configurationCompleteForTarget = true;
    snapshot.configurationFresh = true;
    snapshot.profileMatchesExactly = true;
    snapshot.startupPlanAvailable = true;
    snapshot.deterministicStartupPassed = true;
    snapshot.acquisition = GuidedTunerAcquisitionState::Running;
    snapshot.cleanupVerified = true;

    const auto inputs = MakeGuidedTunerInputs(snapshot);
    CHECK(inputs.projectActive);
    CHECK(inputs.targetSupported);
    CHECK(inputs.ownership == GuidedTunerOwnershipState::SessionOpen);
    CHECK(inputs.mvmeHandoffConfirmed);
    CHECK(inputs.recoveryRecordAvailable);
    CHECK(inputs.activeOperation == GuidedTunerOperation::StartupPreparation);
    CHECK(inputs.profileLoadedForTarget);
    CHECK(inputs.startupAuditCompleteForTarget);
    CHECK(inputs.startupAuditReady);
    CHECK(inputs.configurationCompleteForTarget);
    CHECK(inputs.configurationFresh);
    CHECK(inputs.profileMatchesExactly);
    CHECK(inputs.startupPlanAvailable);
    CHECK(inputs.deterministicStartupPassed);
    CHECK(inputs.acquisition == GuidedTunerAcquisitionState::Running);
    CHECK(inputs.cleanupVerified);
}

TEST_CASE("ownership commands remain explicit semantic types")
{
    using namespace fidget;

    TunerCommand command = CheckStatusCommand{};
    CHECK(std::holds_alternative<CheckStatusCommand>(command));

    command = SetMvmeHandoffConfirmedCommand{true};
    REQUIRE(std::holds_alternative<SetMvmeHandoffConfirmedCommand>(command));
    CHECK(std::get<SetMvmeHandoffConfirmedCommand>(command).confirmed);

    command = RunStartupAuditCommand{};
    CHECK(std::holds_alternative<RunStartupAuditCommand>(command));

    UseCrateProjectCommand useProject;
    useProject.project.mvlcHost = "mvlc-test";
    command = useProject;
    REQUIRE(std::holds_alternative<UseCrateProjectCommand>(command));
    CHECK(std::get<UseCrateProjectCommand>(command).project.mvlcHost
          == "mvlc-test");
}
