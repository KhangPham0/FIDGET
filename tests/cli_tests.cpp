#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "cli/CliApp.h"
#include "core/ScpRegistry.h"
#include "core/StartupAudit.h"
#include "core/StartupPreparation.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <sstream>
#include <utility>
#include <vector>

namespace {

fidget::StartupAuditResult MakeAuditResult(bool blocked)
{
    std::array<
        std::uint16_t,
        fidget::Fw2051StartupAuditRegisterCount> values{};
    for (std::size_t index = 0U;
         index < fidget::Fw2051StartupAuditRegisterTable.size(); ++index)
    {
        switch (
            fidget::Fw2051StartupAuditRegisterTable[index].registerOffset)
        {
        case 0x6008U:
            values[index] = 0x5007U;
            break;
        case 0x600EU:
            values[index] = 0x2051U;
            break;
        case 0x6010U:
            values[index] = 1U;
            break;
        case 0x6018U:
            values[index] = 1U;
            break;
        case 0x601CU:
            values[index] = 1U;
            break;
        case 0x6036U:
            values[index] = 3U;
            break;
        case 0x6044U:
            values[index] = blocked ? 8U : 24U;
            break;
        default:
            break;
        }
    }
    return fidget::ClassifyFw2051StartupAudit(0x11000000U, values);
}

std::vector<fidget::Fw2051StartupPreparationMismatch>
PreparationMismatches(const fidget::StartupAuditResult& audit)
{
    std::array<
        std::uint16_t,
        fidget::Fw2051StartupPreparationRegisterCount> values{};
    for (std::size_t index = 0U; index < values.size(); ++index)
    {
        const auto registerOffset =
            fidget::Fw2051StartupPreparationRegisterTable[index]
                .registerOffset;
        const auto found = std::find_if(
            audit.rows.begin(),
            audit.rows.end(),
            [registerOffset](const fidget::StartupAuditRow& row) {
                return row.registerOffset == registerOffset;
            });
        REQUIRE(found != audit.rows.end());
        values[index] = found->value;
    }
    return fidget::FindFw2051StartupPreparationMismatches(values);
}

fidget::Fw2051ScpConfigurationSnapshot MakeConfiguration(bool differs)
{
    using namespace fidget;

    Fw2051ScpConfigurationSnapshot configuration;
    configuration.state = ScpConfigurationState::Complete;
    configuration.message = "Captured all eight SCP channel quads.";
    configuration.baseAddress = 0x11000000U;
    configuration.hardwareId = Mdpp32HardwareId;
    configuration.firmwareRevision = Mdpp32ScpFirmwareRevisionFw2051;
    configuration.irqLevel = 1U;
    configuration.outputFormat = 0x18U;
    configuration.selectorParkedAtQuadZero = true;
    for (std::uint16_t quadIndex = 0U;
         quadIndex < Fw2051ScpQuadCount;
         ++quadIndex)
    {
        Fw2051ScpQuadConfiguration quad;
        quad.quad = quadIndex;
        quad.timingFilter = 8U;
        quad.poleZero = {2000U, 2010U, 2020U, 2030U};
        quad.gain = 200U;
        quad.thresholds = {1000U, 1001U, 1002U, 1003U};
        quad.shapingTime = 160U;
        quad.baselineRestorer = 2U;
        quad.resetTime = 16U;
        quad.signalRiseTime = 4U;
        quad.preSamples = 50U;
        quad.totalSamples = 400U;
        quad.sampleConfiguration = 0U;
        configuration.quads.push_back(quad);
        configuration.selectorWrites.push_back({
            Fw2051ScpSelectorRegister,
            quadIndex,
            false,
            true,
            "Selector write completed.",
        });
    }
    configuration.selectorWrites.push_back({
        Fw2051ScpSelectorRegister,
        0U,
        true,
        true,
        "Selector write completed.",
    });
    if (differs)
    {
        configuration.quads[5].thresholds[1] = 4321U;
    }
    return configuration;
}

std::size_t CountOccurrences(
    const std::string& text,
    const std::string& pattern)
{
    std::size_t count = 0U;
    std::size_t position = 0U;
    while ((position = text.find(pattern, position)) != std::string::npos)
    {
        ++count;
        position += pattern.size();
    }
    return count;
}

class FakeTunerControl final : public fidget::ITunerControl
{
public:
    explicit FakeTunerControl(
        fidget::GuidedTunerOwnershipState checkResult =
            fidget::GuidedTunerOwnershipState::Idle,
        bool auditBlocked = false,
        bool captureDiffers = false)
        : snapshot_(std::make_shared<const fidget::TunerSnapshot>())
        , checkResult_(checkResult)
        , auditBlocked_(auditBlocked)
        , captureDiffers_(captureDiffers)
    {
    }

    std::shared_ptr<const fidget::TunerSnapshot>
    CurrentSnapshot() const override
    {
        return snapshot_;
    }

    void Submit(fidget::TunerCommand command) override
    {
        auto next = *snapshot_;
        ++next.revision;
        if (const auto* project =
                std::get_if<fidget::UseCrateProjectCommand>(&command))
        {
            ++projectCommands;
            next.projectActive = true;
            next.mvlcHost = project->project.mvlcHost;
            next.mvlcCommandPort = project->project.mvlcCommandPort;
            next.activeModuleIndex = project->activeModuleIndex;
            next.activeModuleName =
                project->project.modules[project->activeModuleIndex].name;
            next.activeModuleBaseAddress =
                project->project.modules[project->activeModuleIndex]
                    .baseAddress;
            next.activeModuleProfilePath =
                project->project.modules[project->activeModuleIndex]
                    .profilePath;
            next.targetSupported = true;
        }
        else if (std::holds_alternative<fidget::CheckStatusCommand>(command))
        {
            ++statusCommands;
            next.ownership = checkResult_;
            next.controllerReadingsValid =
                checkResult_ ==
                fidget::GuidedTunerOwnershipState::Idle;
            next.mvlcHardwareId = 0x5008U;
            next.mvlcFirmwareRevision = 0x0046U;
            next.mvlcDaqMode = checkResult_
                    == fidget::GuidedTunerOwnershipState::Idle
                ? 0U
                : 0x000FU;
            next.statusMessages = {{
                checkResult_ == fidget::GuidedTunerOwnershipState::Idle
                    ? fidget::TunerStatusLevel::Success
                    : fidget::TunerStatusLevel::Warning,
                checkResult_ == fidget::GuidedTunerOwnershipState::Idle
                    ? "Status check passed."
                    : "MVLC DAQ mode is active.",
                {},
            }};
        }
        else if (const auto* handoff =
                     std::get_if<fidget::SetMvmeHandoffConfirmedCommand>(
                         &command))
        {
            ++handoffCommands;
            next.mvmeHandoffConfirmed = handoff->confirmed;
        }
        else if (std::holds_alternative<fidget::OpenSessionCommand>(command))
        {
            ++openCommands;
            next.ownership = fidget::GuidedTunerOwnershipState::SessionOpen;
        }
        else if (std::holds_alternative<
                     fidget::RunStartupAuditCommand>(command))
        {
            ++auditCommands;
            next.startupAudit = MakeAuditResult(auditBlocked_);
            next.startupAuditCompleteForTarget = true;
            next.startupAuditReady =
                next.startupAudit.readyForDiagnosticStart;
        }
        else if (std::holds_alternative<
                     fidget::CaptureConfigurationCommand>(command))
        {
            ++captureCommands;
            next.configurationCapture =
                MakeConfiguration(captureDiffers_);
            next.configurationCompleteForTarget = true;
            next.configurationFresh = true;
            if (next.profileLoadedForTarget)
            {
                next.configurationComparison =
                    fidget::CompareFw2051ScpConfiguration(
                        next.loadedProfile,
                        next.configurationCapture);
                next.profileMatchesExactly =
                    next.configurationComparison.comparable &&
                    next.configurationComparison.differences.empty();
                next.profileApplicationPlan =
                    fidget::PlanFw2051ScpProfileApplication(
                        next.loadedProfile,
                        next.configurationCapture);
                next.standaloneStartupPlan =
                    fidget::PlanFw2051ScpStandaloneStartup(
                        next.loadedProfile,
                        next.configurationCapture);
                if (next.startupAuditCompleteForTarget)
                {
                    next.startupPreparationMismatches =
                        PreparationMismatches(next.startupAudit);
                }
                next.startupPlanAvailable =
                    next.startupAuditCompleteForTarget &&
                    next.standaloneStartupPlan.success;
            }
        }
        else if (const auto* save =
                     std::get_if<fidget::SaveProfileCommand>(&command))
        {
            ++saveCommands;
            savedPath = save->path;
            next.statusMessages = {{
                fidget::TunerStatusLevel::Success,
                "Saved the read-only SCP profile.",
                {},
            }};
        }
        else if (const auto* load =
                     std::get_if<fidget::LoadProfileCommand>(&command))
        {
            ++loadCommands;
            loadedPath = load->path;
            next.profileLoaded = true;
            next.profileLoadedForTarget = true;
            next.loadedProfilePath = load->path;
            next.loadedProfile.configuration = MakeConfiguration(false);
            next.loadedProfile.configuration.selectorWrites.clear();
            next.statusMessages = {{
                fidget::TunerStatusLevel::Success,
                "Loaded the SCP profile.",
                {},
            }};
        }
        else if (const auto* apply =
                     std::get_if<fidget::ApplyProfileRowCommand>(&command))
        {
            ++applyCommands;
            next.singleRepairResult = {};
            next.singleRepairResult.state =
                fidget::ScpSingleRepairState::Passed;
            next.singleRepairResult.message =
                "Applied and retained one profile value.";
            next.singleRepairResult.selectedQuad = apply->quad;
            next.singleRepairResult.registerOffset = apply->registerOffset;
            next.singleRepairResult.writeAttempted = true;
            next.singleRepairResult.writeVerified = true;
            next.singleRepairResult.selectorParkedAtQuadZero = true;
            next.singleRepairResult.profileValueRetained = true;
            next.configurationFresh = false;
        }
        else if (std::holds_alternative<
                     fidget::ApplyAllDifferencesCommand>(command))
        {
            ++applyAllCommands;
            next.bulkApplyResult = {};
            next.bulkApplyResult.state = fidget::ScpBulkApplyState::Passed;
            next.bulkApplyResult.message =
                "Applied and retained all profile values.";
            next.bulkApplyResult.plannedWrites =
                next.profileApplicationPlan.request.steps.size();
            next.bulkApplyResult.writesAttempted =
                next.bulkApplyResult.plannedWrites;
            next.bulkApplyResult.writesVerified =
                next.bulkApplyResult.plannedWrites;
            next.bulkApplyResult.fullPreflightMatched = true;
            next.bulkApplyResult.moduleStopSent = true;
            next.bulkApplyResult.moduleStopVerified = true;
            next.bulkApplyResult.moduleLeftStopped = true;
            next.bulkApplyResult.selectorParkedAtQuadZero = true;
            next.bulkApplyResult.fifoResetSent = true;
            next.bulkApplyResult.readoutResetSent = true;
            next.bulkApplyResult.profileValuesRetained = true;
            for (const auto& step :
                 next.profileApplicationPlan.request.steps)
            {
                fidget::ScpAppliedValueResult value;
                value.quad = step.quad;
                value.registerOffset = step.registerOffset;
                value.settingName = step.settingName;
                value.expectedValue = step.expectedValue;
                value.profileValue = step.profileValue;
                value.writeAttempted = true;
                value.writeVerified = true;
                value.profileValueRetained = true;
                next.bulkApplyResult.values.push_back(std::move(value));
            }
            next.configurationFresh = false;
        }
        else if (const auto* startup = std::get_if<
                     fidget::RunDeterministicStartupCommand>(&command))
        {
            ++startupCommands;
            next.deterministicStartupResult = {};
            auto& result = next.deterministicStartupResult;
            result.state = startup->confirmed
                ? fidget::DeterministicStartupState::Passed
                : fidget::DeterministicStartupState::Failed;
            result.message = startup->confirmed
                ? "Deterministic tuner startup passed: the readout "
                  "contract and all 141 saved SCP values are verified. "
                  "Ready and stopped."
                : "Deterministic startup requires explicit confirmation.";
            result.baseAddress = next.activeModuleBaseAddress;
            result.reviewedPlan = next.standaloneStartupPlan;
            result.valuesCompared =
                next.standaloneStartupPlan.valuesCompared;
            result.initialDifferences =
                next.standaloneStartupPlan.configurationDifferences;
            result.startupContractDifferences =
                next.standaloneStartupPlan.startupContractDifferences;
            result.bankedDifferences =
                next.standaloneStartupPlan.bankedDifferences;
            result.bankedWritesPlanned =
                next.standaloneStartupPlan.bankedApplication.steps.size();
            result.startupPreparationPassed = startup->confirmed;
            result.preparation.state = startup->confirmed
                ? fidget::StartupPreparationState::Passed
                : fidget::StartupPreparationState::Failed;
            result.preparation.changedSettings =
                next.startupPreparationMismatches.size();
            result.preparation.writesAttempted =
                result.preparation.changedSettings;
            result.preparation.writesVerified =
                result.preparation.changedSettings;
            result.preparation.moduleLeftStopped = startup->confirmed;
            result.postPreparationCapturePassed = startup->confirmed;
            result.bankedApplicationNeeded =
                result.bankedWritesPlanned != 0U;
            result.bankedApplicationPassed = startup->confirmed;
            result.finalConfiguration = next.loadedProfile.configuration;
            result.finalComparison = fidget::CompareFw2051ScpConfiguration(
                next.loadedProfile,
                result.finalConfiguration);
            result.finalProfileVerified = startup->confirmed;
            result.moduleLeftStopped = startup->confirmed;
            next.deterministicStartupPassed = startup->confirmed;
            if (startup->confirmed)
            {
                next.configurationCapture = result.finalConfiguration;
                next.configurationCompleteForTarget = true;
                next.configurationFresh = true;
                next.configurationComparison = result.finalComparison;
                next.profileMatchesExactly = true;
            }
        }
        else if (std::holds_alternative<fidget::ReleaseSessionCommand>(command))
        {
            ++releaseCommands;
            next.ownership = fidget::GuidedTunerOwnershipState::Disconnected;
            next.mvmeHandoffConfirmed = false;
        }
        snapshot_ = std::make_shared<const fidget::TunerSnapshot>(
            std::move(next));
    }

    int projectCommands = 0;
    int statusCommands = 0;
    int handoffCommands = 0;
    int openCommands = 0;
    int auditCommands = 0;
    int captureCommands = 0;
    int saveCommands = 0;
    int loadCommands = 0;
    int applyCommands = 0;
    int applyAllCommands = 0;
    int startupCommands = 0;
    int releaseCommands = 0;
    std::string savedPath;
    std::string loadedPath;

private:
    std::shared_ptr<const fidget::TunerSnapshot> snapshot_;
    fidget::GuidedTunerOwnershipState checkResult_;
    bool auditBlocked_ = false;
    bool captureDiffers_ = false;
};

} // namespace

TEST_CASE("CLI options accept project and host status forms")
{
    {
        const char* arguments[] = {
            "fidget_cli", "status", "--host", "mvlc-test",
            "--port", "32769", "--module", "2",
        };
        const auto parsed = fidget::ParseCliOptions(8, arguments);
        REQUIRE(parsed.success);
        CHECK(parsed.options.host == "mvlc-test");
        REQUIRE(parsed.options.port);
        CHECK(*parsed.options.port == 32769U);
        CHECK(parsed.options.moduleIndex == 1U);
    }
    {
        const char* arguments[] = {
            "fidget_cli", "session", "--host", "mvlc-test",
        };
        const auto parsed = fidget::ParseCliOptions(4, arguments);
        REQUIRE(parsed.success);
        CHECK(parsed.options.command == fidget::CliCommand::Session);
    }
    {
        const char* arguments[] = {
            "fidget_cli", "audit", "--host", "mvlc-test",
        };
        const auto parsed = fidget::ParseCliOptions(4, arguments);
        REQUIRE(parsed.success);
        CHECK(parsed.options.command == fidget::CliCommand::Audit);
    }
    {
        const char* arguments[] = {
            "fidget_cli", "status", "--project", "crate.mwwcrate",
        };
        const auto parsed = fidget::ParseCliOptions(4, arguments);
        REQUIRE(parsed.success);
        CHECK(parsed.options.projectPath == "crate.mwwcrate");
    }
    {
        const char* arguments[] = {
            "fidget_cli", "capture", "--host", "mvlc-test",
            "--save", "captured.mwwscp",
        };
        const auto parsed = fidget::ParseCliOptions(6, arguments);
        REQUIRE(parsed.success);
        CHECK(parsed.options.command == fidget::CliCommand::Capture);
        CHECK(parsed.options.savePath == "captured.mwwscp");
    }
    {
        const char* arguments[] = {
            "fidget_cli", "compare", "--host", "mvlc-test",
            "--profile", "expected.mwwscp",
        };
        const auto parsed = fidget::ParseCliOptions(6, arguments);
        REQUIRE(parsed.success);
        CHECK(parsed.options.command == fidget::CliCommand::Compare);
        CHECK(parsed.options.profilePath == "expected.mwwscp");
    }
    {
        const char* arguments[] = {
            "fidget_cli", "apply", "--host", "mvlc-test",
            "--profile", "expected.mwwscp", "--register", "0x611A",
            "--quad", "7",
        };
        const auto parsed = fidget::ParseCliOptions(10, arguments);
        REQUIRE(parsed.success);
        CHECK(parsed.options.command == fidget::CliCommand::Apply);
        REQUIRE(parsed.options.registerOffset);
        CHECK(*parsed.options.registerOffset == 0x611AU);
        REQUIRE(parsed.options.quad);
        CHECK(*parsed.options.quad == 7U);
    }
    {
        const char* arguments[] = {
            "fidget_cli", "apply-all", "--host", "mvlc-test",
            "--profile", "expected.mwwscp",
        };
        const auto parsed = fidget::ParseCliOptions(6, arguments);
        REQUIRE(parsed.success);
        CHECK(parsed.options.command == fidget::CliCommand::ApplyAll);
    }
    {
        const char* arguments[] = {
            "fidget_cli", "startup", "--host", "mvlc-test",
            "--profile", "expected.mwwscp",
        };
        const auto parsed = fidget::ParseCliOptions(6, arguments);
        REQUIRE(parsed.success);
        CHECK(parsed.options.command == fidget::CliCommand::Startup);
        CHECK(parsed.options.profilePath == "expected.mwwscp");
    }
}

TEST_CASE("CLI options reject unsafe or incomplete status arguments")
{
    const char* missingTarget[] = {"fidget_cli", "status"};
    CHECK_FALSE(fidget::ParseCliOptions(2, missingTarget).success);

    const char* zeroPort[] = {
        "fidget_cli", "status", "--host", "mvlc-test", "--port", "0",
    };
    CHECK_FALSE(fidget::ParseCliOptions(6, zeroPort).success);

    const char* zeroModule[] = {
        "fidget_cli", "status", "--host", "mvlc-test", "--module", "0",
    };
    CHECK_FALSE(fidget::ParseCliOptions(6, zeroModule).success);

    const char* compareWithoutProfile[] = {
        "fidget_cli", "compare", "--host", "mvlc-test",
    };
    CHECK_FALSE(
        fidget::ParseCliOptions(4, compareWithoutProfile).success);

    const char* saveOnStatus[] = {
        "fidget_cli", "status", "--host", "mvlc-test",
        "--save", "capture.mwwscp",
    };
    CHECK_FALSE(fidget::ParseCliOptions(6, saveOnStatus).success);

    const char* applyWithoutRegister[] = {
        "fidget_cli", "apply", "--host", "mvlc-test",
        "--profile", "expected.mwwscp", "--quad", "7",
    };
    CHECK_FALSE(
        fidget::ParseCliOptions(8, applyWithoutRegister).success);

    const char* invalidQuad[] = {
        "fidget_cli", "apply", "--host", "mvlc-test",
        "--profile", "expected.mwwscp", "--register", "0x611A",
        "--quad", "8",
    };
    CHECK_FALSE(fidget::ParseCliOptions(10, invalidQuad).success);
}

TEST_CASE("status prints idle readings and exits successfully")
{
    fidget::CliOptions options;
    options.host = "mvlc-test";
    FakeTunerControl control;
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliStatus(
        options, control, output, errors, [] { return false; });

    CHECK(exitCode == 0);
    CHECK(control.projectCommands == 1);
    CHECK(control.statusCommands == 1);
    CHECK(errors.str().empty());
    CHECK(output.str().find("ownership: idle\n") != std::string::npos);
    CHECK(output.str().find("mvlc_hardware_id: 0x00005008\n")
          != std::string::npos);
    CHECK(output.str().find("mvlc_firmware_revision: 0x00000046\n")
          != std::string::npos);
    CHECK(output.str().find("mvlc_daq_mode: 0x00000000\n")
          != std::string::npos);
}

TEST_CASE("status classifies an active DAQ as a failure exit")
{
    fidget::CliOptions options;
    options.host = "mvlc-test";
    FakeTunerControl control(
        fidget::GuidedTunerOwnershipState::InUse);
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliStatus(
        options, control, output, errors, [] { return false; });

    CHECK(exitCode == 1);
    CHECK(control.statusCommands == 1);
    CHECK(output.str().find("ownership: in-use\n") != std::string::npos);
    CHECK(output.str().find("mvlc_hardware_id: not read\n")
          != std::string::npos);
    CHECK(output.str().find("mvlc_daq_mode: 0x0000000F\n")
          != std::string::npos);
}

TEST_CASE("status honors SIGINT only after the command completes")
{
    fidget::CliOptions options;
    options.host = "mvlc-test";
    FakeTunerControl control;
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliStatus(
        options, control, output, errors, [] { return true; });

    CHECK(exitCode == 130);
    CHECK(control.statusCommands == 1);
    CHECK(output.str().find("ownership: idle\n") != std::string::npos);
}

TEST_CASE("session defaults to no when confirmation input is closed")
{
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Session;
    options.host = "mvlc-test";
    FakeTunerControl control;
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliSession(
        options,
        control,
        input,
        output,
        errors,
        [] { return false; },
        [] { return fidget::CliSessionWaitResult::InputReady; });

    CHECK(exitCode == 1);
    CHECK(control.statusCommands == 1);
    CHECK(control.handoffCommands == 0);
    CHECK(control.openCommands == 0);
    CHECK(control.releaseCommands == 0);
    CHECK(output.str().find("session: not opened\n")
          != std::string::npos);
}

TEST_CASE("session prints watchdog state and releases on Enter")
{
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Session;
    options.host = "mvlc-test";
    FakeTunerControl control;
    std::istringstream input("yes\n\n");
    std::ostringstream output;
    std::ostringstream errors;
    int waits = 0;

    const int exitCode = fidget::RunCliSession(
        options,
        control,
        input,
        output,
        errors,
        [] { return false; },
        [&waits] {
            ++waits;
            return waits == 1
                ? fidget::CliSessionWaitResult::OneSecondElapsed
                : fidget::CliSessionWaitResult::InputReady;
        });

    CHECK(exitCode == 0);
    CHECK(control.handoffCommands == 1);
    CHECK(control.openCommands == 1);
    CHECK(control.releaseCommands == 1);
    CHECK(output.str().find(
              "watchdog: ownership=session-open daq_mode=0x00000000\n")
          != std::string::npos);
    CHECK(output.str().find("session: released\n") != std::string::npos);
}

TEST_CASE("session releases after EOF while monitoring")
{
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Session;
    options.host = "mvlc-test";
    FakeTunerControl control;
    std::istringstream input("y\n");
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliSession(
        options,
        control,
        input,
        output,
        errors,
        [] { return false; },
        [] { return fidget::CliSessionWaitResult::InputReady; });

    CHECK(exitCode == 0);
    CHECK(control.openCommands == 1);
    CHECK(control.releaseCommands == 1);
}

TEST_CASE("SIGINT after open is honored only after release")
{
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Session;
    options.host = "mvlc-test";
    FakeTunerControl control;
    std::istringstream input("yes\n");
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliSession(
        options,
        control,
        input,
        output,
        errors,
        [&control] { return control.openCommands > 0; },
        [] { return fidget::CliSessionWaitResult::OneSecondElapsed; });

    CHECK(exitCode == 130);
    CHECK(control.openCommands == 1);
    CHECK(control.releaseCommands == 1);
    CHECK(output.str().find("session: released\n") != std::string::npos);
}

TEST_CASE("SIGINT during release is reported after release completes")
{
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Session;
    options.host = "mvlc-test";
    FakeTunerControl control;
    std::istringstream input("yes\n\n");
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliSession(
        options,
        control,
        input,
        output,
        errors,
        [&control] { return control.releaseCommands > 0; },
        [] { return fidget::CliSessionWaitResult::InputReady; });

    CHECK(exitCode == 130);
    CHECK(control.openCommands == 1);
    CHECK(control.releaseCommands == 1);
    CHECK(output.str().find("session: released\n") != std::string::npos);
}

TEST_CASE("audit prints all rows and releases a ready session")
{
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Audit;
    options.host = "mvlc-test";
    FakeTunerControl control;
    std::istringstream input("yes\n");
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliAudit(
        options,
        control,
        input,
        output,
        errors,
        [] { return false; });

    CHECK(exitCode == 0);
    CHECK(control.statusCommands == 1);
    CHECK(control.handoffCommands == 1);
    CHECK(control.openCommands == 1);
    CHECK(control.auditCommands == 1);
    CHECK(control.releaseCommands == 1);
    CHECK(errors.str().empty());
    CHECK(CountOccurrences(output.str(), "\n0x") == 37U);
    CHECK(output.str().find(
              "audit_summary: required=7/7 blocking=0 warnings=0 "
              "ready=yes\n") != std::string::npos);
    CHECK(output.str().find("session: released\n") != std::string::npos);
}

TEST_CASE("audit releases and fails when a required check is blocked")
{
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Audit;
    options.host = "mvlc-test";
    FakeTunerControl control(
        fidget::GuidedTunerOwnershipState::Idle, true);
    std::istringstream input("y\n");
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliAudit(
        options,
        control,
        input,
        output,
        errors,
        [] { return false; });

    CHECK(exitCode == 1);
    CHECK(control.auditCommands == 1);
    CHECK(control.releaseCommands == 1);
    CHECK(output.str().find(
              "audit_summary: required=6/7 blocking=1 warnings=0 "
              "ready=no\n") != std::string::npos);
    CHECK(output.str().find("session: released\n") != std::string::npos);
}

TEST_CASE("audit defers SIGINT until its session is released")
{
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Audit;
    options.host = "mvlc-test";
    FakeTunerControl control;
    std::istringstream input("yes\n");
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliAudit(
        options,
        control,
        input,
        output,
        errors,
        [&control] { return control.auditCommands > 0; });

    CHECK(exitCode == 130);
    CHECK(control.auditCommands == 1);
    CHECK(control.releaseCommands == 1);
    CHECK(output.str().find("session: released\n") != std::string::npos);
}

TEST_CASE("audit defaults to no when confirmation input is closed")
{
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Audit;
    options.host = "mvlc-test";
    FakeTunerControl control;
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliAudit(
        options,
        control,
        input,
        output,
        errors,
        [] { return false; });

    CHECK(exitCode == 1);
    CHECK(control.openCommands == 0);
    CHECK(control.auditCommands == 0);
    CHECK(control.releaseCommands == 0);
}

TEST_CASE("capture prints 141 values, logs selector writes, and saves")
{
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Capture;
    options.host = "mvlc-test";
    options.savePath = "captured.mwwscp";
    FakeTunerControl control;
    std::istringstream input("yes\n");
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliCapture(
        options,
        control,
        input,
        output,
        errors,
        [] { return false; });

    CHECK(exitCode == 0);
    CHECK(control.statusCommands == 1);
    CHECK(control.openCommands == 1);
    CHECK(control.captureCommands == 1);
    CHECK(control.saveCommands == 1);
    CHECK(control.savedPath == "captured.mwwscp");
    CHECK(control.releaseCommands == 1);
    CHECK(errors.str().empty());
    CHECK(CountOccurrences(output.str(), "\nvalue ") == 141U);
    CHECK(CountOccurrences(output.str(), "\nselector_write ") == 9U);
    CHECK(output.str().find(
              "capture_summary: values=141 selector_writes=9 "
              "selector_parked=yes\n") != std::string::npos);
    CHECK(output.str().find("profile_saved: captured.mwwscp\n") !=
          std::string::npos);
    CHECK(output.str().find("session: released\n") != std::string::npos);
}

TEST_CASE("compare exits successfully only for an identical profile")
{
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Compare;
    options.host = "mvlc-test";
    options.profilePath = "expected.mwwscp";
    FakeTunerControl control;
    std::istringstream input("yes\n");
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliCompare(
        options,
        control,
        input,
        output,
        errors,
        [] { return false; });

    CHECK(exitCode == 0);
    CHECK(control.loadCommands == 1);
    CHECK(control.loadedPath == "expected.mwwscp");
    CHECK(control.captureCommands == 1);
    CHECK(control.releaseCommands == 1);
    CHECK(errors.str().empty());
    CHECK(output.str().find(
              "compare_summary: comparable=yes compared=141 "
              "differences=0 identical=yes\n") != std::string::npos);
}

TEST_CASE("compare prints complete attribution for every difference")
{
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Compare;
    options.host = "mvlc-test";
    options.profilePath = "expected.mwwscp";
    FakeTunerControl control(
        fidget::GuidedTunerOwnershipState::Idle, false, true);
    std::istringstream input("y\n");
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliCompare(
        options,
        control,
        input,
        output,
        errors,
        [] { return false; });

    CHECK(exitCode == 1);
    CHECK(control.captureCommands == 1);
    CHECK(control.releaseCommands == 1);
    CHECK(errors.str().empty());
    CHECK(output.str().find(
              "difference quad=5 register=0x611E "
              "setting=\"Threshold ch 1\" profile=1001 live=4321\n") !=
          std::string::npos);
    CHECK(output.str().find(
              "compare_summary: comparable=yes compared=141 "
              "differences=1 identical=no\n") != std::string::npos);
}

TEST_CASE("capture defers SIGINT until the session is released")
{
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Capture;
    options.host = "mvlc-test";
    FakeTunerControl control;
    std::istringstream input("yes\n");
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliCapture(
        options,
        control,
        input,
        output,
        errors,
        [&control] { return control.captureCommands > 0; });

    CHECK(exitCode == 130);
    CHECK(control.captureCommands == 1);
    CHECK(control.releaseCommands == 1);
    CHECK(output.str().find("session: released\n") != std::string::npos);
}

TEST_CASE("apply prints one exact plan and retains the selected value")
{
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Apply;
    options.host = "mvlc-test";
    options.profilePath = "expected.mwwscp";
    options.registerOffset = 0x611EU;
    options.quad = 5U;
    FakeTunerControl control(
        fidget::GuidedTunerOwnershipState::Idle, false, true);
    std::istringstream input("yes\ny\n");
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliApply(
        options,
        control,
        input,
        output,
        errors,
        [] { return false; });

    CHECK(exitCode == 0);
    CHECK(control.captureCommands == 1);
    CHECK(control.applyCommands == 1);
    CHECK(control.applyAllCommands == 0);
    CHECK(control.releaseCommands == 1);
    CHECK(errors.str().empty());
    CHECK(output.str().find("plan: writes=1\n") != std::string::npos);
    CHECK(output.str().find(
              "plan_step quad=5 register=0x611E "
              "setting=\"Threshold ch 1\" live=4321 profile=1001\n") !=
          std::string::npos);
    CHECK(output.str().find(
              "transaction_result: state=passed write=verified "
              "rollback=none retained=profile parked=yes\n") !=
          std::string::npos);
    CHECK(output.str().find(
              "stale_reminder: recapture all eight quads before comparing "
              "or applying another value\n") != std::string::npos);
    CHECK(output.str().find("session: released\n") != std::string::npos);
}

TEST_CASE("apply defaults to no at a closed confirmation input")
{
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Apply;
    options.host = "mvlc-test";
    options.profilePath = "expected.mwwscp";
    options.registerOffset = 0x611EU;
    options.quad = 5U;
    FakeTunerControl control(
        fidget::GuidedTunerOwnershipState::Idle, false, true);
    std::istringstream input("yes\n");
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliApply(
        options,
        control,
        input,
        output,
        errors,
        [] { return false; });

    CHECK(exitCode == 1);
    CHECK(control.captureCommands == 1);
    CHECK(control.applyCommands == 0);
    CHECK(control.releaseCommands == 1);
    CHECK(output.str().find("transaction: not applied\n") !=
          std::string::npos);
    CHECK(output.str().find("session: released\n") != std::string::npos);
}

TEST_CASE("apply-all prints its count and releases after retained values")
{
    fidget::CliOptions options;
    options.command = fidget::CliCommand::ApplyAll;
    options.host = "mvlc-test";
    options.profilePath = "expected.mwwscp";
    FakeTunerControl control(
        fidget::GuidedTunerOwnershipState::Idle, false, true);
    std::istringstream input("yes\ny\n");
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliApply(
        options,
        control,
        input,
        output,
        errors,
        [] { return false; });

    CHECK(exitCode == 0);
    CHECK(control.applyCommands == 0);
    CHECK(control.applyAllCommands == 1);
    CHECK(control.releaseCommands == 1);
    CHECK(errors.str().empty());
    CHECK(output.str().find(
              "Apply 1 banked profile write(s) [y/N]: ") !=
          std::string::npos);
    CHECK(output.str().find(
              "transaction_summary: state=passed planned=1 written=1 "
              "rolled_back=0 retained=yes module_stopped=yes parked=yes\n") !=
          std::string::npos);
}

TEST_CASE("SIGINT during apply is reported only after session release")
{
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Apply;
    options.host = "mvlc-test";
    options.profilePath = "expected.mwwscp";
    options.registerOffset = 0x611EU;
    options.quad = 5U;
    FakeTunerControl control(
        fidget::GuidedTunerOwnershipState::Idle, false, true);
    std::istringstream input("yes\ny\n");
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliApply(
        options,
        control,
        input,
        output,
        errors,
        [&control] { return control.applyCommands > 0; });

    CHECK(exitCode == 130);
    CHECK(control.applyCommands == 1);
    CHECK(control.releaseCommands == 1);
    CHECK(output.str().find("session: released\n") != std::string::npos);
}

TEST_CASE("startup prints its full recipe and ends ready and stopped")
{
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Startup;
    options.host = "mvlc-test";
    options.profilePath = "expected.mwwscp";
    FakeTunerControl control(
        fidget::GuidedTunerOwnershipState::Idle, false, true);
    std::istringstream input("yes\ny\n");
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliStartup(
        options,
        control,
        input,
        output,
        errors,
        [] { return false; });

    CHECK(exitCode == 0);
    CHECK(control.loadCommands == 1);
    CHECK(control.openCommands == 1);
    CHECK(control.auditCommands == 1);
    CHECK(control.captureCommands == 1);
    CHECK(control.startupCommands == 1);
    CHECK(control.releaseCommands == 1);
    CHECK(errors.str().empty());
    CHECK(CountOccurrences(output.str(), "recipe_step: ") == 5U);
    CHECK(output.str().find(
              "startup_recipe_summary: compared=141 differences=1 "
              "preparation_mismatches=5 profile_contract_mismatches=0 "
              "banked_writes=1\n") != std::string::npos);
    CHECK(output.str().find(
              "plan_step quad=5 register=0x611E "
              "setting=\"Threshold ch 1\" live=4321 profile=1001\n") !=
          std::string::npos);
    CHECK(output.str().find(
              "Run deterministic startup with 5 preparation change(s) "
              "and 1 banked write(s) [y/N]: ") != std::string::npos);
    CHECK(output.str().find(
              "startup_summary: state=passed preparation_writes=5/5 "
              "banked_writes=1 final_values=141/141 verified=yes "
              "module_stopped=yes\n") != std::string::npos);
    CHECK(output.str().find("Ready and stopped.\n") != std::string::npos);
    CHECK(output.str().find("session: released\n") != std::string::npos);
}

TEST_CASE("startup defaults to no and releases when confirmation is closed")
{
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Startup;
    options.host = "mvlc-test";
    options.profilePath = "expected.mwwscp";
    FakeTunerControl control(
        fidget::GuidedTunerOwnershipState::Idle, false, true);
    std::istringstream input("yes\n");
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliStartup(
        options,
        control,
        input,
        output,
        errors,
        [] { return false; });

    CHECK(exitCode == 1);
    CHECK(control.auditCommands == 1);
    CHECK(control.captureCommands == 1);
    CHECK(control.startupCommands == 0);
    CHECK(control.releaseCommands == 1);
    CHECK(output.str().find("startup: not run\n") != std::string::npos);
    CHECK(output.str().find("session: released\n") != std::string::npos);
}

TEST_CASE("SIGINT during startup is reported only after session release")
{
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Startup;
    options.host = "mvlc-test";
    options.profilePath = "expected.mwwscp";
    FakeTunerControl control(
        fidget::GuidedTunerOwnershipState::Idle, false, true);
    std::istringstream input("yes\ny\n");
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliStartup(
        options,
        control,
        input,
        output,
        errors,
        [&control] { return control.startupCommands > 0; });

    CHECK(exitCode == 130);
    CHECK(control.startupCommands == 1);
    CHECK(control.releaseCommands == 1);
    CHECK(output.str().find("session: released\n") != std::string::npos);
}
