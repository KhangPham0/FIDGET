#include "cli/CliApp.h"

#include "core/CrateProject.h"
#include "core/StartupAudit.h"

#include <charconv>
#include <chrono>
#include <iomanip>
#include <istream>
#include <limits>
#include <ostream>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

namespace fidget {
namespace {

using namespace std::chrono_literals;

constexpr auto CommandCompletionTimeout = std::chrono::seconds(15);

bool ParseUnsigned(
    std::string_view text,
    std::uint64_t maximum,
    std::uint64_t& value)
{
    if (text.empty())
    {
        return false;
    }

    std::uint64_t parsed = 0U;
    const auto result = std::from_chars(
        text.data(), text.data() + text.size(), parsed, 10);
    if (result.ec != std::errc{}
        || result.ptr != text.data() + text.size()
        || parsed > maximum)
    {
        return false;
    }
    value = parsed;
    return true;
}

bool WaitForRevision(
    ITunerControl& tunerControl,
    std::uint64_t previousRevision,
    const std::function<bool(const TunerSnapshot&)>& ready)
{
    const auto deadline =
        std::chrono::steady_clock::now() + CommandCompletionTimeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto snapshot = tunerControl.CurrentSnapshot();
        if (snapshot->revision > previousRevision && ready(*snapshot))
        {
            return true;
        }
        std::this_thread::sleep_for(10ms);
    }
    return false;
}

CrateProject MakeHostOnlyProject(
    const std::string& host,
    std::uint16_t port)
{
    CrateProject project;
    project.mvlcHost = host;
    project.mvlcCommandPort = port;
    project.streamHost = host;
    project.streamPort = 42333U;
    project.modules.push_back({
        "MDPP-32 SCP",
        0x11000000U,
        MdppBackend::Scp,
        "not-used-by-controller-status.mwwscp",
    });
    return project;
}

const char* OwnershipClassification(GuidedTunerOwnershipState ownership)
{
    switch (ownership)
    {
    case GuidedTunerOwnershipState::Disconnected:
        return "disconnected";
    case GuidedTunerOwnershipState::Checking:
        return "checking";
    case GuidedTunerOwnershipState::Idle:
        return "idle";
    case GuidedTunerOwnershipState::InUse:
        return "in-use";
    case GuidedTunerOwnershipState::SessionOpen:
        return "session-open";
    case GuidedTunerOwnershipState::OwnershipLost:
        return "ownership-lost";
    case GuidedTunerOwnershipState::RecoveryRequired:
        return "recovery-required";
    case GuidedTunerOwnershipState::Failed:
        return "failed";
    }
    return "unknown";
}

void PrintHexReading(
    std::ostream& output,
    const char* name,
    std::uint32_t value)
{
    output << name << ": 0x"
           << std::uppercase << std::hex << std::setw(8)
           << std::setfill('0') << value
           << std::dec << std::nouppercase << std::setfill(' ') << '\n';
}

void PrintStartupAudit(
    std::ostream& output,
    const StartupAuditResult& audit)
{
    output << "audit_rows:\n";
    for (const auto& row : audit.rows)
    {
        output << "0x"
               << std::uppercase << std::hex << std::setw(4)
               << std::setfill('0') << row.registerOffset
               << " 0x" << std::setw(4) << row.value
               << std::dec << std::nouppercase << std::setfill(' ')
               << ' ' << StartupAuditRoleName(row.role)
               << ' ' << StartupAuditAssessmentName(row.assessment)
               << ' ' << row.name << ": " << row.note << '\n';
    }
    output << "audit_summary: required="
           << audit.requiredReady << '/' << audit.requiredChecks
           << " blocking=" << audit.blockingIssues
           << " warnings=" << audit.warnings
           << " ready="
           << (audit.readyForDiagnosticStart ? "yes" : "no")
           << '\n';
}

bool ResolveProject(
    const CliOptions& options,
    CrateProject& project,
    std::ostream& errorOutput)
{
    if (!options.projectPath.empty())
    {
        const auto loaded = LoadCrateProject(options.projectPath);
        if (!loaded.success || !loaded.project)
        {
            errorOutput << "error: " << loaded.message << '\n';
            return false;
        }
        project = *loaded.project;
    }
    else
    {
        project = MakeHostOnlyProject(
            options.host, options.port.value_or(32768U));
    }

    if (!options.host.empty())
    {
        project.mvlcHost = options.host;
    }
    if (options.port)
    {
        project.mvlcCommandPort = *options.port;
    }

    const auto validation = ValidateCrateProject(project);
    if (!validation.success)
    {
        errorOutput << "error: " << validation.message << '\n';
        return false;
    }
    if (options.moduleIndex >= project.modules.size())
    {
        errorOutput << "error: module " << options.moduleIndex + 1U
                    << " is outside the crate project\n";
        return false;
    }
    return true;
}

bool HandoffConfirmedByUser(std::string response)
{
    const auto first = response.find_first_not_of(" \t\r");
    if (first == std::string::npos)
    {
        return false;
    }
    const auto last = response.find_last_not_of(" \t\r");
    response = response.substr(first, last - first + 1U);
    for (char& character : response)
    {
        if (character >= 'A' && character <= 'Z')
        {
            character = static_cast<char>(character - 'A' + 'a');
        }
    }
    return response == "y" || response == "yes";
}

bool ReleaseSession(
    ITunerControl& tunerControl,
    std::ostream& errorOutput)
{
    const auto beforeRelease = tunerControl.CurrentSnapshot()->revision;
    tunerControl.Submit(ReleaseSessionCommand{});
    if (!WaitForRevision(
            tunerControl,
            beforeRelease,
            [](const TunerSnapshot& snapshot) {
                return snapshot.ownership
                    == GuidedTunerOwnershipState::Disconnected;
            }))
    {
        errorOutput << "error: session release timed out\n";
        return false;
    }
    return true;
}

} // namespace

const char* FidgetCliUsage() noexcept
{
    return
        "Usage:\n"
        "  fidget_cli status (--project FILE | --host HOST) [options]\n"
        "  fidget_cli session (--project FILE | --host HOST) [options]\n"
        "  fidget_cli audit (--project FILE | --host HOST) [options]\n"
        "\n"
        "Options:\n"
        "  --project FILE  Load a crate project\n"
        "  --host HOST     Use or override the MVLC host\n"
        "  --port PORT     Use or override the MVLC command port\n"
        "  --module N      Select the one-based project module (default 1)\n"
        "  -h, --help      Show this help\n";
}

CliOptionsParseResult ParseCliOptions(
    int argumentCount,
    const char* const* arguments)
{
    CliOptionsParseResult result;
    if (argumentCount < 2)
    {
        result.error = "missing command";
        return result;
    }

    const std::string_view command = arguments[1];
    if (command == "-h" || command == "--help")
    {
        result.success = true;
        result.options.showHelp = true;
        return result;
    }
    if (command == "status")
    {
        result.options.command = CliCommand::Status;
    }
    else if (command == "session")
    {
        result.options.command = CliCommand::Session;
    }
    else if (command == "audit")
    {
        result.options.command = CliCommand::Audit;
    }
    else
    {
        result.error = "unknown command '" + std::string(command) + "'";
        return result;
    }

    for (int index = 2; index < argumentCount; ++index)
    {
        const std::string_view option = arguments[index];
        if (option == "-h" || option == "--help")
        {
            result.options.showHelp = true;
            continue;
        }
        if (option != "--project"
            && option != "--host"
            && option != "--port"
            && option != "--module")
        {
            result.error = "unknown option '" + std::string(option) + "'";
            return result;
        }
        if (++index >= argumentCount)
        {
            result.error = "missing value after '" + std::string(option) + "'";
            return result;
        }

        const std::string value = arguments[index];
        if (option == "--project")
        {
            result.options.projectPath = value;
        }
        else if (option == "--host")
        {
            result.options.host = value;
        }
        else if (option == "--port")
        {
            std::uint64_t port = 0U;
            if (!ParseUnsigned(
                    value,
                    std::numeric_limits<std::uint16_t>::max(),
                    port)
                || port == 0U)
            {
                result.error = "invalid command port '" + value + "'";
                return result;
            }
            result.options.port = static_cast<std::uint16_t>(port);
        }
        else
        {
            std::uint64_t module = 0U;
            if (!ParseUnsigned(
                    value,
                    std::numeric_limits<std::size_t>::max(),
                    module)
                || module == 0U)
            {
                result.error = "invalid module number '" + value + "'";
                return result;
            }
            result.options.moduleIndex = static_cast<std::size_t>(module - 1U);
        }
    }

    if (result.options.projectPath.empty() && result.options.host.empty()
        && !result.options.showHelp)
    {
        result.error = std::string(command)
            + " requires --project FILE or --host HOST";
        return result;
    }

    result.success = true;
    return result;
}

int RunCliStatus(
    const CliOptions& options,
    ITunerControl& tunerControl,
    std::ostream& output,
    std::ostream& errorOutput,
    const CliInterruptRequested& interruptRequested)
{
    CrateProject project;
    if (!ResolveProject(options, project, errorOutput))
    {
        return 1;
    }

    const auto beforeProject = tunerControl.CurrentSnapshot()->revision;
    UseCrateProjectCommand useProject;
    useProject.projectPath = options.projectPath;
    useProject.project = std::move(project);
    useProject.activeModuleIndex = options.moduleIndex;
    tunerControl.Submit(std::move(useProject));
    if (!WaitForRevision(
            tunerControl,
            beforeProject,
            [](const TunerSnapshot& snapshot) {
                return snapshot.projectActive;
            }))
    {
        errorOutput << "error: project activation timed out\n";
        return 1;
    }

    const auto beforeCheck = tunerControl.CurrentSnapshot()->revision;
    tunerControl.Submit(CheckStatusCommand{});
    if (!WaitForRevision(
            tunerControl,
            beforeCheck,
            [](const TunerSnapshot& snapshot) {
                return snapshot.ownership
                    != GuidedTunerOwnershipState::Checking;
            }))
    {
        errorOutput << "error: MVLC status check timed out\n";
        return 1;
    }

    const auto snapshot = tunerControl.CurrentSnapshot();
    output << "ownership: "
           << OwnershipClassification(snapshot->ownership) << '\n';
    if (snapshot->controllerReadingsValid)
    {
        PrintHexReading(
            output, "mvlc_hardware_id", snapshot->mvlcHardwareId);
    }
    else
    {
        output << "mvlc_hardware_id: not read\n";
    }
    PrintHexReading(
        output, "mvlc_firmware_revision", snapshot->mvlcFirmwareRevision);
    PrintHexReading(output, "mvlc_daq_mode", snapshot->mvlcDaqMode);
    if (!snapshot->statusMessages.empty())
    {
        output << "message: "
               << snapshot->statusMessages.back().summary << '\n';
    }

    if (interruptRequested && interruptRequested())
    {
        return 130;
    }
    return snapshot->ownership == GuidedTunerOwnershipState::Idle ? 0 : 1;
}

int RunCliSession(
    const CliOptions& options,
    ITunerControl& tunerControl,
    std::istream& input,
    std::ostream& output,
    std::ostream& errorOutput,
    const CliInterruptRequested& interruptRequested,
    const CliWaitForSessionInput& waitForInput)
{
    const int statusResult = RunCliStatus(
        options,
        tunerControl,
        output,
        errorOutput,
        interruptRequested);
    if (statusResult != 0)
    {
        return statusResult;
    }

    output << "I stopped the MVME run and completely quit MVME [y/N]: "
           << std::flush;
    std::string confirmation;
    if (!std::getline(input, confirmation)
        || !HandoffConfirmedByUser(std::move(confirmation)))
    {
        output << "session: not opened\n";
        return interruptRequested && interruptRequested() ? 130 : 1;
    }

    const auto beforeConfirmation =
        tunerControl.CurrentSnapshot()->revision;
    tunerControl.Submit(SetMvmeHandoffConfirmedCommand{true});
    if (!WaitForRevision(
            tunerControl,
            beforeConfirmation,
            [](const TunerSnapshot& snapshot) {
                return snapshot.mvmeHandoffConfirmed;
            }))
    {
        errorOutput << "error: handoff confirmation timed out\n";
        return 1;
    }
    if (interruptRequested && interruptRequested())
    {
        return 130;
    }

    const auto beforeOpen = tunerControl.CurrentSnapshot()->revision;
    tunerControl.Submit(OpenSessionCommand{});
    if (!WaitForRevision(
            tunerControl,
            beforeOpen,
            [](const TunerSnapshot& snapshot) {
                return snapshot.ownership
                    != GuidedTunerOwnershipState::Checking;
            }))
    {
        errorOutput << "error: session open timed out\n";
        return 1;
    }

    auto snapshot = tunerControl.CurrentSnapshot();
    if (snapshot->ownership != GuidedTunerOwnershipState::SessionOpen)
    {
        errorOutput << "error: session open failed";
        if (!snapshot->statusMessages.empty())
        {
            errorOutput << ": " << snapshot->statusMessages.back().summary;
        }
        errorOutput << '\n';
        return 1;
    }

    output << "session: open\n" << std::flush;
    bool sessionFailed = false;
    while (!(interruptRequested && interruptRequested()))
    {
        const auto waitResult = waitForInput();
        if (waitResult == CliSessionWaitResult::Interrupted)
        {
            if (interruptRequested && interruptRequested())
            {
                break;
            }
            continue;
        }
        if (waitResult == CliSessionWaitResult::Error)
        {
            errorOutput << "error: waiting for terminal input failed\n";
            sessionFailed = true;
            break;
        }
        if (waitResult == CliSessionWaitResult::InputReady)
        {
            std::string ignored;
            (void)std::getline(input, ignored);
            break;
        }

        snapshot = tunerControl.CurrentSnapshot();
        output << "watchdog: ownership="
               << OwnershipClassification(snapshot->ownership)
               << " daq_mode=0x"
               << std::uppercase << std::hex << std::setw(8)
               << std::setfill('0') << snapshot->mvlcDaqMode
               << std::dec << std::nouppercase << std::setfill(' ')
               << '\n' << std::flush;
        if (snapshot->ownership
            != GuidedTunerOwnershipState::SessionOpen)
        {
            sessionFailed = true;
            break;
        }
    }

    const bool interrupted = interruptRequested && interruptRequested();
    if (!ReleaseSession(tunerControl, errorOutput))
    {
        return 1;
    }
    output << "session: released\n";
    if (interrupted || (interruptRequested && interruptRequested()))
    {
        return 130;
    }
    return sessionFailed ? 1 : 0;
}

int RunCliAudit(
    const CliOptions& options,
    ITunerControl& tunerControl,
    std::istream& input,
    std::ostream& output,
    std::ostream& errorOutput,
    const CliInterruptRequested& interruptRequested)
{
    const int statusResult = RunCliStatus(
        options,
        tunerControl,
        output,
        errorOutput,
        interruptRequested);
    if (statusResult != 0)
    {
        return statusResult;
    }

    output << "I stopped the MVME run and completely quit MVME [y/N]: "
           << std::flush;
    std::string confirmation;
    if (!std::getline(input, confirmation)
        || !HandoffConfirmedByUser(std::move(confirmation)))
    {
        output << "session: not opened\n";
        return interruptRequested && interruptRequested() ? 130 : 1;
    }

    const auto beforeConfirmation =
        tunerControl.CurrentSnapshot()->revision;
    tunerControl.Submit(SetMvmeHandoffConfirmedCommand{true});
    if (!WaitForRevision(
            tunerControl,
            beforeConfirmation,
            [](const TunerSnapshot& snapshot) {
                return snapshot.mvmeHandoffConfirmed;
            }))
    {
        errorOutput << "error: handoff confirmation timed out\n";
        return 1;
    }
    if (interruptRequested && interruptRequested())
    {
        return 130;
    }

    const auto beforeOpen = tunerControl.CurrentSnapshot()->revision;
    tunerControl.Submit(OpenSessionCommand{});
    if (!WaitForRevision(
            tunerControl,
            beforeOpen,
            [](const TunerSnapshot& snapshot) {
                return snapshot.ownership
                    != GuidedTunerOwnershipState::Checking;
            }))
    {
        errorOutput << "error: session open timed out\n";
        return 1;
    }

    auto snapshot = tunerControl.CurrentSnapshot();
    if (snapshot->ownership != GuidedTunerOwnershipState::SessionOpen)
    {
        errorOutput << "error: session open failed";
        if (!snapshot->statusMessages.empty())
        {
            errorOutput << ": " << snapshot->statusMessages.back().summary;
        }
        errorOutput << '\n';
        return 1;
    }
    output << "session: open\n" << std::flush;

    int result = 1;
    if (!(interruptRequested && interruptRequested()))
    {
        const auto beforeAudit = snapshot->revision;
        tunerControl.Submit(RunStartupAuditCommand{});
        if (!WaitForRevision(
                tunerControl,
                beforeAudit,
                [](const TunerSnapshot& value) {
                    return value.startupAudit.state
                            == StartupAuditState::Complete
                        || value.startupAudit.state
                            == StartupAuditState::Failed;
                }))
        {
            errorOutput << "error: startup audit timed out\n";
        }
        else
        {
            snapshot = tunerControl.CurrentSnapshot();
            if (snapshot->startupAudit.state == StartupAuditState::Complete)
            {
                PrintStartupAudit(output, snapshot->startupAudit);
                result = snapshot->startupAudit.blockingIssues == 0U
                    ? 0
                    : 1;
            }
            else
            {
                errorOutput << "error: startup audit failed: "
                            << snapshot->startupAudit.message << '\n';
            }
        }
    }

    const bool interrupted = interruptRequested && interruptRequested();
    if (!ReleaseSession(tunerControl, errorOutput))
    {
        return 1;
    }
    output << "session: released\n";
    if (interrupted || (interruptRequested && interruptRequested()))
    {
        return 130;
    }
    return result;
}

} // namespace fidget
