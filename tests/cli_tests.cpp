#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "cli/CliApp.h"

#include <memory>
#include <sstream>
#include <utility>

namespace {

class FakeTunerControl final : public fidget::ITunerControl
{
public:
    explicit FakeTunerControl(
        fidget::GuidedTunerOwnershipState checkResult =
            fidget::GuidedTunerOwnershipState::Idle)
        : snapshot_(std::make_shared<const fidget::TunerSnapshot>())
        , checkResult_(checkResult)
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
            next.targetSupported = true;
        }
        else if (std::holds_alternative<fidget::CheckStatusCommand>(command))
        {
            ++statusCommands;
            next.ownership = checkResult_;
            next.controllerReadingsValid = true;
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
    int releaseCommands = 0;

private:
    std::shared_ptr<const fidget::TunerSnapshot> snapshot_;
    fidget::GuidedTunerOwnershipState checkResult_;
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
            "fidget_cli", "status", "--project", "crate.mwwcrate",
        };
        const auto parsed = fidget::ParseCliOptions(4, arguments);
        REQUIRE(parsed.success);
        CHECK(parsed.options.projectPath == "crate.mwwcrate");
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
