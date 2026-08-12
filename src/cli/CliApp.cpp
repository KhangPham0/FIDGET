#include "cli/CliApp.h"

#include "core/CrateProject.h"
#include "core/DeterministicStartup.h"
#include "core/ScpProfile.h"
#include "core/ScpRegistry.h"
#include "core/ScpTransactionPlan.h"
#include "core/StartupAudit.h"
#include "core/StartupPreparation.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <fstream>
#include <istream>
#include <limits>
#include <ostream>
#include <sstream>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace fidget {
namespace {

using namespace std::chrono_literals;

constexpr auto CommandCompletionTimeout = std::chrono::seconds(15);
constexpr auto StartupCompletionTimeout = std::chrono::minutes(2);
constexpr auto AcquisitionCompletionTimeout = std::chrono::minutes(2);

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

bool ParseRegisterOffset(
    std::string_view text,
    std::uint16_t& value)
{
    int base = 10;
    if (text.size() > 2U && text[0] == '0' &&
        (text[1] == 'x' || text[1] == 'X'))
    {
        text.remove_prefix(2U);
        base = 16;
    }
    if (text.empty())
    {
        return false;
    }

    std::uint32_t parsed = 0U;
    const auto result = std::from_chars(
        text.data(), text.data() + text.size(), parsed, base);
    if (result.ec != std::errc{} ||
        result.ptr != text.data() + text.size() || parsed > 0xFFFFU)
    {
        return false;
    }
    value = static_cast<std::uint16_t>(parsed);
    return true;
}

bool WaitForRevision(
    ITunerControl& tunerControl,
    std::uint64_t previousRevision,
    const std::function<bool(const TunerSnapshot&)>& ready,
    const std::chrono::steady_clock::duration timeout =
        CommandCompletionTimeout)
{
    const auto deadline =
        std::chrono::steady_clock::now() + timeout;
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

bool TransactionConfirmedByUser(std::string response)
{
    const auto first = response.find_first_not_of(" \t\r");
    if (first == std::string::npos)
    {
        return false;
    }
    const auto last = response.find_last_not_of(" \t\r");
    response = response.substr(first, last - first + 1U);
    return response == "y" || response == "Y";
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

int ConfirmAndOpenSession(
    ITunerControl& tunerControl,
    std::istream& input,
    std::ostream& output,
    std::ostream& errorOutput,
    const CliInterruptRequested& interruptRequested)
{
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

    const auto snapshot = tunerControl.CurrentSnapshot();
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
    return 0;
}

int ReleaseOperationSession(
    ITunerControl& tunerControl,
    std::ostream& output,
    std::ostream& errorOutput,
    const CliInterruptRequested& interruptRequested,
    int operationResult)
{
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
    return operationResult;
}

void PrintConfigurationValue(
    std::ostream& output,
    const std::string& scope,
    const std::string& registerName,
    const std::string& setting,
    std::uint32_t value,
    bool hexadecimal)
{
    output << "value scope=" << scope
           << " register=" << registerName
           << " setting=\"" << setting << "\" value=";
    if (hexadecimal)
    {
        output << "0x" << std::uppercase << std::hex
               << std::setw(value > 0xFFFFU ? 8 : 4)
               << std::setfill('0') << value
               << std::dec << std::nouppercase << std::setfill(' ');
    }
    else
    {
        output << value;
    }
    output << '\n';
}

void PrintSelectorWrites(
    std::ostream& output,
    const Fw2051ScpConfigurationSnapshot& configuration)
{
    output << "selector_writes:\n";
    for (const auto& write : configuration.selectorWrites)
    {
        output << "selector_write register=0x"
               << std::uppercase << std::hex << std::setw(4)
               << std::setfill('0') << write.registerOffset
               << std::dec << std::nouppercase << std::setfill(' ')
               << " value=" << write.value
               << " purpose="
               << (write.parkingWrite ? "parking" : "bank-selection")
               << " result=" << (write.success ? "success" : "failed");
        if (!write.success)
        {
            output << " message=\"" << write.message << '"';
        }
        output << '\n';
    }
}

void PrintScpConfiguration(
    std::ostream& output,
    const Fw2051ScpConfigurationSnapshot& configuration)
{
    PrintSelectorWrites(output, configuration);
    output << "configuration_values:\n";
    PrintConfigurationValue(
        output,
        "global",
        "metadata",
        "VME base",
        configuration.baseAddress,
        true);
    PrintConfigurationValue(
        output, "global", "0x6008", "Hardware ID",
        configuration.hardwareId, true);
    PrintConfigurationValue(
        output, "global", "0x600E", "Firmware revision",
        configuration.firmwareRevision, true);
    PrintConfigurationValue(
        output, "global", "0x6010", "IRQ level",
        configuration.irqLevel, false);
    PrintConfigurationValue(
        output, "global", "0x6044", "Output format",
        configuration.outputFormat, true);

    for (const auto& quad : configuration.quads)
    {
        for (const auto& definition : Fw2051ScpSettingRegistry)
        {
            const auto value = Fw2051ScpQuadRegisterValue(
                quad, definition.registerOffset);
            if (!value)
            {
                continue;
            }
            char registerName[8]{};
            std::snprintf(
                registerName,
                sizeof(registerName),
                "0x%04X",
                static_cast<unsigned>(definition.registerOffset));
            PrintConfigurationValue(
                output,
                "quad-" + std::to_string(quad.quad),
                registerName,
                definition.name,
                *value,
                definition.registerOffset == 0x614AU);
        }
    }
    output << "capture_summary: values="
           << Fw2051ScpConfigurationValueCount
           << " selector_writes=" << configuration.selectorWrites.size()
           << " selector_parked="
           << (configuration.selectorParkedAtQuadZero ? "yes" : "no")
           << '\n';
}

void PrintComparison(
    std::ostream& output,
    const ScpConfigurationComparison& comparison)
{
    output << "differences:\n";
    for (const auto& difference : comparison.differences)
    {
        output << "difference quad=";
        if (difference.quad < 0)
        {
            output << "global";
        }
        else
        {
            output << difference.quad;
        }
        output << " register=";
        if (difference.hasRegister)
        {
            output << "0x" << std::uppercase << std::hex << std::setw(4)
                   << std::setfill('0') << difference.registerOffset
                   << std::dec << std::nouppercase << std::setfill(' ');
        }
        else
        {
            output << "metadata";
        }
        output << " setting=\"" << difference.setting << "\" profile=";
        if (difference.displayHexadecimal)
        {
            output << "0x" << std::uppercase << std::hex
                   << std::setw(difference.profileValue > 0xFFFFU ? 8 : 4)
                   << std::setfill('0') << difference.profileValue
                   << " live=0x"
                   << std::setw(difference.liveValue > 0xFFFFU ? 8 : 4)
                   << difference.liveValue
                   << std::dec << std::nouppercase << std::setfill(' ');
        }
        else
        {
            output << difference.profileValue
                   << " live=" << difference.liveValue;
        }
        output << '\n';
    }
    output << "compare_summary: comparable="
           << (comparison.comparable ? "yes" : "no")
           << " compared=" << comparison.valuesCompared
           << " differences=" << comparison.differences.size()
           << " identical="
           << (comparison.comparable && comparison.differences.empty()
                   ? "yes"
                   : "no")
           << '\n';
}

void PrintApplicationStep(
    std::ostream& output,
    const ScpProfileApplicationStep& step)
{
    output << "plan_step quad=" << step.quad
           << " register=0x" << std::uppercase << std::hex
           << std::setw(4) << std::setfill('0') << step.registerOffset
           << std::dec << std::nouppercase << std::setfill(' ')
           << " setting=\"" << step.settingName << "\" live="
           << step.expectedValue << " profile=" << step.profileValue
           << '\n';
}

void PrintSingleRepairResult(
    std::ostream& output,
    const ScpSingleRepairResult& result)
{
    output << "transaction_result: state="
           << (result.state == ScpSingleRepairState::Passed
                   ? "passed"
                   : "failed")
           << " write=" << (result.writeVerified ? "verified" :
                              result.writeAttempted ? "unverified" : "none")
           << " rollback="
           << (result.rollbackVerified ? "verified" :
               result.rollbackAttempted ? "unverified" : "none")
           << " retained="
           << (result.profileValueRetained ? "profile" : "not-profile")
           << " parked="
           << (result.selectorParkedAtQuadZero ? "yes" : "no")
           << '\n';
    output << "transaction_message: " << result.message << '\n';
}

void PrintBulkApplyResult(
    std::ostream& output,
    const ScpBulkApplyResult& result)
{
    output << "transaction_values:\n";
    for (const auto& value : result.values)
    {
        output << "transaction_value quad=" << value.quad
               << " register=0x" << std::uppercase << std::hex
               << std::setw(4) << std::setfill('0')
               << value.registerOffset
               << std::dec << std::nouppercase << std::setfill(' ')
               << " setting=\"" << value.settingName << "\" write="
               << (value.writeVerified ? "verified" :
                   value.writeAttempted ? "unverified" : "none")
               << " rollback="
               << (value.rollbackVerified ? "verified" :
                   value.rollbackAttempted ? "unverified" : "none")
               << " retained="
               << (value.profileValueRetained ? "profile" : "not-profile")
               << '\n';
    }
    output << "transaction_summary: state="
           << (result.state == ScpBulkApplyState::Passed
                   ? "passed"
                   : "failed")
           << " planned=" << result.plannedWrites
           << " written=" << result.writesVerified
           << " rolled_back=" << result.rollbackWritesVerified
           << " retained="
           << (result.profileValuesRetained ? "yes" : "no")
           << " module_stopped="
           << (result.moduleLeftStopped ? "yes" : "no")
           << " parked="
           << (result.selectorParkedAtQuadZero ? "yes" : "no")
           << '\n';
    output << "transaction_message: " << result.message << '\n';
}

void PrintStartupRecipe(
    std::ostream& output,
    const TunerSnapshot& snapshot)
{
    const auto& plan = snapshot.standaloneStartupPlan;
    output << "startup_recipe:\n"
           << "recipe_step: prepare and verify the eight-register "
              "module-wide readout contract\n"
           << "recipe_step: recapture all eight SCP banks before "
              "planning a write\n"
           << "recipe_step: apply the fresh banked plan with exact "
              "readback and rollback protection\n"
           << "recipe_step: recapture and prove all 141 saved values\n"
           << "recipe_step: leave acquisition stopped\n";
    for (const auto& mismatch : snapshot.startupPreparationMismatches)
    {
        output << "preparation_step register=0x"
               << std::uppercase << std::hex << std::setw(4)
               << std::setfill('0') << mismatch.registerOffset
               << std::dec << std::nouppercase << std::setfill(' ')
               << " setting=\"" << mismatch.name << "\" live="
               << mismatch.currentValue
               << " required=" << mismatch.targetValue << '\n';
    }
    for (const auto& step : plan.bankedApplication.steps)
    {
        PrintApplicationStep(output, step);
    }
    output << "startup_recipe_summary: compared=" << plan.valuesCompared
           << " differences=" << plan.configurationDifferences
           << " preparation_mismatches="
           << snapshot.startupPreparationMismatches.size()
           << " profile_contract_mismatches="
           << plan.startupContractDifferences
           << " banked_writes=" << plan.bankedApplication.steps.size()
           << '\n'
           << "startup_recipe_message: " << plan.message << '\n';
}

void PrintDeterministicStartupResult(
    std::ostream& output,
    const DeterministicStartupResult& result)
{
    output << "startup_summary: state="
           << (result.state == DeterministicStartupState::Passed
                   ? "passed"
                   : "failed")
           << " preparation_writes="
           << result.preparation.writesVerified << '/'
           << result.preparation.changedSettings
           << " banked_writes=" << result.bankedWritesPlanned
           << " final_values=" << result.finalComparison.valuesCompared
           << "/141 verified="
           << (result.finalProfileVerified ? "yes" : "no")
           << " module_stopped="
           << (result.moduleLeftStopped ? "yes" : "no")
           << '\n';
    output << "startup_message: " << result.message << '\n';
}

void PrintAcquisitionStatus(
    std::ostream& output,
    const TunerSnapshot& snapshot)
{
    const auto& stream = snapshot.diagnosticStream;
    std::uint64_t requestedCount = 0U;
    if (stream.requestedTarget.moduleObserved)
    {
        const auto found = stream.histories.find({
            static_cast<std::uint8_t>(stream.requestedTarget.moduleId),
            stream.requestedTarget.requestedChannel,
        });
        if (found != stream.histories.end())
        {
            requestedCount = found->second.totalCaptured;
        }
    }
    output << "acquisition_status: packets="
           << stream.decoderStats.ethernetPackets
           << " datagrams=" << stream.datagramsReceived
           << " waveforms=" << stream.decoderStats.decodedWaveforms
           << " loss=" << stream.decoderStats.lostEthernetPackets
           << " decode_errors=" << stream.decoderStats.malformedWords
           << " channel=" << stream.requestedChannel
           << " channel_count=" << requestedCount
           << " fingerprint="
           << (snapshot.diagnosticAcquisition.communicationUncertain
                   ? "uncertain"
                   : snapshot.diagnosticAcquisition
                         .foreignControllerDetected
                       ? "foreign"
                       : "matching")
           << '\n';
    output << "acquisition_channels:";
    for (const auto& channel : stream.channelWaveformTotals)
    {
        output << ' ' << channel.channel << '=' << channel.total;
    }
    output << '\n';
}

void PrintAcquisitionIsolation(
    std::ostream& output,
    const DiagnosticAcquisitionResult& result)
{
    output << "acquisition_isolation: checked="
           << result.moduleIsolation.size()
           << " quiesced=" << result.nonTargetModulesQuiesced << '\n';
    for (const auto& module : result.moduleIsolation)
    {
        output << "isolation_start: base=0x"
               << std::uppercase << std::hex << std::setw(8)
               << std::setfill('0') << module.baseAddress
               << " hardware=0x" << std::setw(4) << module.hardwareId
               << std::dec << std::nouppercase << std::setfill(' ')
               << " irq=" << module.irqLevel
               << " state_before=" << module.acquisitionStateBefore
               << " quiesced="
               << (module.stopVerified && module.fifoResetSent
                       && module.readoutResetSent
                       ? "yes"
                       : "no")
               << " stop=" << (module.stopVerified ? "verified" : "failed")
               << " fifo_reset="
               << (module.fifoResetSent ? "yes" : "no")
               << " readout_reset="
               << (module.readoutResetSent ? "yes" : "no")
               << '\n';
    }
}

void PrintCleanupResult(
    std::ostream& output,
    const DiagnosticAcquisitionResult& result)
{
    output << "cleanup_selected: stopped="
           << (result.moduleStopSent ? "yes" : "no") << '\n'
           << "cleanup_isolation: verified="
           << result.nonTargetModulesVerifiedStoppedOnCleanup << '/'
           << result.nonTargetModulesQuiesced << '\n'
           << "cleanup_mvlc: daq_mode_zero="
           << (result.daqModeDisabled ? "yes" : "no")
           << " stack_zero="
           << (result.readoutStackDisabled ? "yes" : "no")
           << " journal_removed="
           << (result.recoveryJournalRemoved ? "yes" : "no") << '\n'
           << "cleanup_message: " << result.message << '\n';
}

void PrintSourceChange(
    std::ostream& output,
    const DiagnosticSourceChangeResult& result)
{
    output << "source_change: state="
           << (result.state == DiagnosticSourceChangeState::Passed
                   ? "passed"
                   : "failed")
           << " quad=" << result.selectedQuad
           << " source=" << static_cast<unsigned>(result.requestedSource)
           << " config=0x" << std::uppercase << std::hex
           << std::setw(4) << std::setfill('0')
           << result.originalConfiguration << "->0x"
           << std::setw(4) << result.requestedConfiguration
           << " readback=0x" << std::setw(4) << result.appliedReadback
           << std::dec << std::nouppercase << std::setfill(' ')
           << " parked="
           << (result.selectorParkedAtQuadZero ? "yes" : "no")
           << " resumed="
           << (result.acquisitionResumed && result.daqModeResumed
                   ? "yes"
                   : "no")
           << '\n'
           << "source_message: " << result.message << '\n';
}

void PrintParameterPreview(
    std::ostream& output,
    const DiagnosticParameterPreviewResult& result)
{
    const char* state = "failed";
    if (result.state == DiagnosticParameterPreviewState::PreviewActive)
    {
        state = "active";
    }
    else if (result.state == DiagnosticParameterPreviewState::Restored)
    {
        state = "restored";
    }
    output << "parameter_preview: state=" << state
           << " quad=" << result.selectedQuad
           << " register=0x" << std::uppercase << std::hex
           << std::setw(4) << std::setfill('0') << result.registerOffset
           << std::dec << std::nouppercase << std::setfill(' ')
           << " setting=\"" << result.settingName << "\""
           << " original=" << result.originalValue
           << " preview=" << result.requestedValue
           << " apply_readback=" << result.appliedReadback
           << " restore_readback=" << result.restoredReadback
           << " apply_us=" << result.applyDurationMicroseconds
           << " restore_us=" << result.restoreDurationMicroseconds
           << '\n'
           << "preview_message: " << result.message << '\n';
}

bool ParseAcquisitionCommand(
    const std::string& line,
    ITunerControl& tunerControl,
    std::ostream& output,
    std::ostream& errorOutput)
{
    if (line.size() == 2U && line[0] == 's'
        && line[1] >= '0' && line[1] <= '3')
    {
        const auto source = static_cast<std::uint8_t>(line[1] - '0');
        const auto before = tunerControl.CurrentSnapshot()->revision;
        tunerControl.Submit(ChangeDiagnosticSourceCommand{source});
        if (!WaitForRevision(
                tunerControl,
                before,
                [](const TunerSnapshot& snapshot) {
                    return snapshot.diagnosticSourceChange.state
                            == DiagnosticSourceChangeState::Passed
                        || snapshot.diagnosticSourceChange.state
                            == DiagnosticSourceChangeState::Failed;
                },
                AcquisitionCompletionTimeout))
        {
            errorOutput << "error: source change timed out\n";
            return false;
        }
        PrintSourceChange(
            output,
            tunerControl.CurrentSnapshot()->diagnosticSourceChange);
        return true;
    }
    if (line == "r")
    {
        const auto before = tunerControl.CurrentSnapshot()->revision;
        tunerControl.Submit(RestoreDiagnosticPreviewCommand{});
        if (!WaitForRevision(
                tunerControl,
                before,
                [](const TunerSnapshot& snapshot) {
                    return snapshot.diagnosticParameterPreview.state
                            == DiagnosticParameterPreviewState::Restored
                        || snapshot.diagnosticParameterPreview.state
                            == DiagnosticParameterPreviewState::Failed;
                },
                AcquisitionCompletionTimeout))
        {
            errorOutput << "error: parameter restore timed out\n";
            return false;
        }
        PrintParameterPreview(
            output,
            tunerControl.CurrentSnapshot()->diagnosticParameterPreview);
        return true;
    }
    if (line.size() > 2U && line[0] == 'p' && line[1] == ' ')
    {
        std::istringstream fields(line.substr(2U));
        std::string registerText;
        std::string valueText;
        std::string trailing;
        if (!(fields >> registerText >> valueText) || fields >> trailing)
        {
            errorOutput << "error: preview syntax is p <register> <value>\n";
            return true;
        }
        std::uint16_t registerOffset = 0U;
        std::uint64_t value = 0U;
        if (!ParseRegisterOffset(registerText, registerOffset)
            || !ParseUnsigned(valueText, 0xFFFFU, value))
        {
            errorOutput << "error: invalid preview register or value\n";
            return true;
        }
        const auto* definition = FindFw2051ScpSetting(registerOffset);
        if (definition == nullptr)
        {
            errorOutput << "error: preview register is not in the FW2051 SCP registry\n";
            return true;
        }
        const auto validation = ValidateFw2051ScpSettingValue(
            *definition, static_cast<std::uint16_t>(value), "preview ");
        if (!validation.empty())
        {
            errorOutput << "error: " << validation << '\n';
            return true;
        }
        const auto before = tunerControl.CurrentSnapshot()->revision;
        tunerControl.Submit(ApplyDiagnosticPreviewCommand{
            registerOffset,
            static_cast<std::uint16_t>(value),
        });
        if (!WaitForRevision(
                tunerControl,
                before,
                [](const TunerSnapshot& snapshot) {
                    return snapshot.diagnosticParameterPreview.state
                            == DiagnosticParameterPreviewState::PreviewActive
                        || snapshot.diagnosticParameterPreview.state
                            == DiagnosticParameterPreviewState::Failed;
                },
                AcquisitionCompletionTimeout))
        {
            errorOutput << "error: parameter preview timed out\n";
            return false;
        }
        PrintParameterPreview(
            output,
            tunerControl.CurrentSnapshot()->diagnosticParameterPreview);
        return true;
    }

    errorOutput
        << "error: acquisition command must be s0-s3, p <register> <value>, r, or an empty line to stop\n";
    return true;
}

bool DumpRequestedWaveformCsv(
    const std::string& path,
    const DiagnosticStreamSnapshot& stream,
    std::string& error)
{
    if (!stream.requestedTarget.moduleObserved)
    {
        error = "No waveform module ID was observed.";
        return false;
    }
    const auto found = stream.histories.find({
        static_cast<std::uint8_t>(stream.requestedTarget.moduleId),
        stream.requestedTarget.requestedChannel,
    });
    if (found == stream.histories.end()
        || found->second.waveforms.empty())
    {
        error = "No waveform was captured for the requested channel.";
        return false;
    }

    std::ofstream csv(path, std::ios::trunc);
    if (!csv)
    {
        error = "Could not open the CSV output file.";
        return false;
    }
    csv << "sample_index,value\n";
    const auto& waveform = found->second.waveforms.back();
    for (std::size_t index = 0U; index < waveform.samples.size(); ++index)
    {
        csv << index << ',' << waveform.samples[index] << '\n';
    }
    csv.flush();
    if (!csv)
    {
        error = "Writing the CSV output file failed.";
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
        "  fidget_cli capture (--project FILE | --host HOST) [options]\n"
        "  fidget_cli compare (--project FILE | --host HOST) --profile FILE "
        "[options]\n"
        "  fidget_cli apply (--project FILE | --host HOST) --profile FILE "
        "--register OFFSET --quad N [options]\n"
        "  fidget_cli apply-all (--project FILE | --host HOST) --profile "
        "FILE [options]\n"
        "  fidget_cli startup (--project FILE | --host HOST) --profile "
        "FILE [options]\n"
        "  fidget_cli acquire --project FILE --channel N [options]\n"
        "\n"
        "Options:\n"
        "  --project FILE  Load a crate project\n"
        "  --host HOST     Use or override the MVLC host\n"
        "  --port PORT     Use or override the MVLC command port\n"
        "  --module N      Select the one-based project module (default 1)\n"
        "  --save FILE     Save a successful capture as an SCP profile\n"
        "  --profile FILE  Load this SCP profile for compare, apply, or "
        "startup; override the project profile for acquire\n"
        "  --register OFF  Select a banked register, decimal or 0x-prefixed\n"
        "  --quad N        Select channel quad 0 through 7\n"
        "  --channel N     Select physical channel 0 through 31\n"
        "  --seconds N     Stop acquisition after N status intervals\n"
        "  --dump-csv FILE Write the latest requested-channel waveform\n"
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
    else if (command == "capture")
    {
        result.options.command = CliCommand::Capture;
    }
    else if (command == "compare")
    {
        result.options.command = CliCommand::Compare;
    }
    else if (command == "apply")
    {
        result.options.command = CliCommand::Apply;
    }
    else if (command == "apply-all")
    {
        result.options.command = CliCommand::ApplyAll;
    }
    else if (command == "startup")
    {
        result.options.command = CliCommand::Startup;
    }
    else if (command == "acquire")
    {
        result.options.command = CliCommand::Acquire;
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
            && option != "--module"
            && option != "--save"
            && option != "--profile"
            && option != "--register"
            && option != "--quad"
            && option != "--channel"
            && option != "--seconds"
            && option != "--dump-csv")
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
        else if (option == "--module")
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
        else if (option == "--save")
        {
            result.options.savePath = value;
        }
        else if (option == "--profile")
        {
            result.options.profilePath = value;
        }
        else if (option == "--register")
        {
            std::uint16_t registerOffset = 0U;
            if (!ParseRegisterOffset(value, registerOffset))
            {
                result.error = "invalid register offset '" + value + "'";
                return result;
            }
            result.options.registerOffset = registerOffset;
        }
        else if (option == "--quad")
        {
            std::uint64_t quad = 0U;
            if (!ParseUnsigned(value, 7U, quad))
            {
                result.error = "invalid channel quad '" + value + "'";
                return result;
            }
            result.options.quad = static_cast<std::uint16_t>(quad);
        }
        else if (option == "--channel")
        {
            std::uint64_t channel = 0U;
            if (!ParseUnsigned(value, 31U, channel))
            {
                result.error = "invalid physical channel '" + value + "'";
                return result;
            }
            result.options.channel = static_cast<std::uint16_t>(channel);
        }
        else if (option == "--seconds")
        {
            std::uint64_t seconds = 0U;
            if (!ParseUnsigned(
                    value,
                    std::numeric_limits<std::uint32_t>::max(),
                    seconds)
                || seconds == 0U)
            {
                result.error = "invalid acquisition duration '" + value + "'";
                return result;
            }
            result.options.seconds = static_cast<std::uint32_t>(seconds);
        }
        else
        {
            result.options.dumpCsvPath = value;
        }
    }

    if (result.options.projectPath.empty() && result.options.host.empty()
        && !result.options.showHelp)
    {
        result.error = std::string(command)
            + " requires --project FILE or --host HOST";
        return result;
    }
    if (!result.options.savePath.empty()
        && result.options.command != CliCommand::Capture)
    {
        result.error = "--save is valid only for the capture command";
        return result;
    }
    if (!result.options.profilePath.empty()
        && result.options.command != CliCommand::Compare
        && result.options.command != CliCommand::Apply
        && result.options.command != CliCommand::ApplyAll
        && result.options.command != CliCommand::Startup
        && result.options.command != CliCommand::Acquire)
    {
        result.error =
            "--profile is valid only for compare, apply, startup, or acquire";
        return result;
    }
    if ((result.options.command == CliCommand::Compare
         || result.options.command == CliCommand::Apply
         || result.options.command == CliCommand::ApplyAll
         || result.options.command == CliCommand::Startup)
        && result.options.profilePath.empty()
        && !result.options.showHelp)
    {
        result.error = std::string(command) + " requires --profile FILE";
        return result;
    }
    if ((result.options.registerOffset || result.options.quad) &&
        result.options.command != CliCommand::Apply)
    {
        result.error = "--register and --quad are valid only for apply";
        return result;
    }
    if (result.options.command == CliCommand::Apply &&
        (!result.options.registerOffset || !result.options.quad) &&
        !result.options.showHelp)
    {
        result.error = "apply requires --register OFFSET and --quad N";
        return result;
    }
    if ((result.options.channel || result.options.seconds
         || !result.options.dumpCsvPath.empty())
        && result.options.command != CliCommand::Acquire)
    {
        result.error =
            "--channel, --seconds, and --dump-csv are valid only for acquire";
        return result;
    }
    if (result.options.command == CliCommand::Acquire
        && !result.options.channel && !result.options.showHelp)
    {
        result.error = "acquire requires --channel N";
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

    const int openResult = ConfirmAndOpenSession(
        tunerControl, input, output, errorOutput, interruptRequested);
    if (openResult != 0)
    {
        return openResult;
    }

    auto snapshot = tunerControl.CurrentSnapshot();
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

    const int openResult = ConfirmAndOpenSession(
        tunerControl, input, output, errorOutput, interruptRequested);
    if (openResult != 0)
    {
        return openResult;
    }

    auto snapshot = tunerControl.CurrentSnapshot();

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

    return ReleaseOperationSession(
        tunerControl,
        output,
        errorOutput,
        interruptRequested,
        result);
}

int RunCliCapture(
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

    const int openResult = ConfirmAndOpenSession(
        tunerControl, input, output, errorOutput, interruptRequested);
    if (openResult != 0)
    {
        return openResult;
    }

    int result = 1;
    if (!(interruptRequested && interruptRequested()))
    {
        const auto beforeCapture =
            tunerControl.CurrentSnapshot()->revision;
        tunerControl.Submit(CaptureConfigurationCommand{});
        if (!WaitForRevision(
                tunerControl,
                beforeCapture,
                [](const TunerSnapshot& snapshot) {
                    return snapshot.configurationCapture.state ==
                            ScpConfigurationState::Complete ||
                        snapshot.configurationCapture.state ==
                            ScpConfigurationState::Failed;
                }))
        {
            errorOutput << "error: SCP configuration capture timed out\n";
        }
        else
        {
            const auto snapshot = tunerControl.CurrentSnapshot();
            if (snapshot->configurationCapture.state ==
                ScpConfigurationState::Complete)
            {
                PrintScpConfiguration(
                    output, snapshot->configurationCapture);
                result = 0;
            }
            else
            {
                PrintSelectorWrites(
                    output, snapshot->configurationCapture);
                errorOutput << "error: SCP configuration capture failed: "
                            << snapshot->configurationCapture.message
                            << '\n';
            }
        }
    }

    if (result == 0 && !options.savePath.empty() &&
        !(interruptRequested && interruptRequested()))
    {
        const auto beforeSave = tunerControl.CurrentSnapshot()->revision;
        tunerControl.Submit(SaveProfileCommand{options.savePath});
        if (!WaitForRevision(
                tunerControl,
                beforeSave,
                [](const TunerSnapshot&) { return true; }))
        {
            errorOutput << "error: SCP profile save timed out\n";
            result = 1;
        }
        else
        {
            const auto saved = tunerControl.CurrentSnapshot();
            const bool saveSucceeded = !saved->statusMessages.empty() &&
                saved->statusMessages.back().level ==
                    TunerStatusLevel::Success;
            if (saveSucceeded)
            {
                output << "profile_saved: " << options.savePath << '\n';
            }
            else
            {
                errorOutput << "error: SCP profile save failed";
                if (!saved->statusMessages.empty())
                {
                    errorOutput << ": "
                                << saved->statusMessages.back().summary;
                }
                errorOutput << '\n';
                result = 1;
            }
        }
    }

    return ReleaseOperationSession(
        tunerControl,
        output,
        errorOutput,
        interruptRequested,
        result);
}

int RunCliCompare(
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

    const auto beforeLoad = tunerControl.CurrentSnapshot()->revision;
    tunerControl.Submit(LoadProfileCommand{options.profilePath});
    if (!WaitForRevision(
            tunerControl,
            beforeLoad,
            [](const TunerSnapshot&) { return true; }))
    {
        errorOutput << "error: SCP profile load timed out\n";
        return 1;
    }
    auto snapshot = tunerControl.CurrentSnapshot();
    if (!snapshot->profileLoadedForTarget)
    {
        errorOutput << "error: SCP profile load failed";
        if (!snapshot->statusMessages.empty())
        {
            errorOutput << ": " << snapshot->statusMessages.back().summary;
        }
        errorOutput << '\n';
        return 1;
    }
    output << "profile_loaded: " << options.profilePath << '\n';
    if (interruptRequested && interruptRequested())
    {
        return 130;
    }

    const int openResult = ConfirmAndOpenSession(
        tunerControl, input, output, errorOutput, interruptRequested);
    if (openResult != 0)
    {
        return openResult;
    }

    int result = 1;
    if (!(interruptRequested && interruptRequested()))
    {
        const auto beforeCapture =
            tunerControl.CurrentSnapshot()->revision;
        tunerControl.Submit(CaptureConfigurationCommand{});
        if (!WaitForRevision(
                tunerControl,
                beforeCapture,
                [](const TunerSnapshot& value) {
                    return value.configurationCapture.state ==
                            ScpConfigurationState::Complete ||
                        value.configurationCapture.state ==
                            ScpConfigurationState::Failed;
                }))
        {
            errorOutput << "error: SCP configuration capture timed out\n";
        }
        else
        {
            snapshot = tunerControl.CurrentSnapshot();
            if (snapshot->configurationCapture.state ==
                ScpConfigurationState::Complete)
            {
                PrintScpConfiguration(
                    output, snapshot->configurationCapture);
                PrintComparison(
                    output, snapshot->configurationComparison);
                result = snapshot->configurationComparison.comparable &&
                        snapshot->configurationComparison.differences.empty()
                    ? 0
                    : 1;
            }
            else
            {
                PrintSelectorWrites(
                    output, snapshot->configurationCapture);
                errorOutput << "error: SCP configuration capture failed: "
                            << snapshot->configurationCapture.message
                            << '\n';
            }
        }
    }

    return ReleaseOperationSession(
        tunerControl,
        output,
        errorOutput,
        interruptRequested,
        result);
}

int RunCliApply(
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

    const auto beforeLoad = tunerControl.CurrentSnapshot()->revision;
    tunerControl.Submit(LoadProfileCommand{options.profilePath});
    if (!WaitForRevision(
            tunerControl,
            beforeLoad,
            [](const TunerSnapshot&) { return true; }))
    {
        errorOutput << "error: SCP profile load timed out\n";
        return 1;
    }
    auto snapshot = tunerControl.CurrentSnapshot();
    if (!snapshot->profileLoadedForTarget)
    {
        errorOutput << "error: SCP profile load failed";
        if (!snapshot->statusMessages.empty())
        {
            errorOutput << ": " << snapshot->statusMessages.back().summary;
        }
        errorOutput << '\n';
        return 1;
    }
    output << "profile_loaded: " << options.profilePath << '\n';
    if (interruptRequested && interruptRequested())
    {
        return 130;
    }

    const int openResult = ConfirmAndOpenSession(
        tunerControl, input, output, errorOutput, interruptRequested);
    if (openResult != 0)
    {
        return openResult;
    }

    int result = 1;
    bool readyToApply = false;
    std::vector<ScpProfileApplicationStep> steps;
    if (!(interruptRequested && interruptRequested()))
    {
        const auto beforeCapture =
            tunerControl.CurrentSnapshot()->revision;
        tunerControl.Submit(CaptureConfigurationCommand{});
        if (!WaitForRevision(
                tunerControl,
                beforeCapture,
                [](const TunerSnapshot& value) {
                    return value.configurationCapture.state ==
                            ScpConfigurationState::Complete ||
                        value.configurationCapture.state ==
                            ScpConfigurationState::Failed;
                }))
        {
            errorOutput << "error: SCP configuration capture timed out\n";
        }
        else
        {
            snapshot = tunerControl.CurrentSnapshot();
            if (snapshot->configurationCapture.state !=
                ScpConfigurationState::Complete)
            {
                errorOutput << "error: SCP configuration capture failed: "
                            << snapshot->configurationCapture.message
                            << '\n';
            }
            else if (!snapshot->configurationComparison.comparable)
            {
                errorOutput << "error: profile comparison failed: "
                            << snapshot->configurationComparison.message
                            << '\n';
            }
            else if (options.command == CliCommand::ApplyAll)
            {
                if (!snapshot->profileApplicationPlan.success)
                {
                    errorOutput << "error: profile application is blocked: "
                                << snapshot->profileApplicationPlan.message
                                << '\n';
                }
                else if (snapshot->profileApplicationPlan.request.steps
                             .empty())
                {
                    errorOutput <<
                        "error: there are no banked profile differences "
                        "to apply\n";
                }
                else
                {
                    steps = snapshot->profileApplicationPlan.request.steps;
                    readyToApply = true;
                }
            }
            else if (!options.registerOffset || !options.quad)
            {
                errorOutput <<
                    "error: apply requires a register and channel quad\n";
            }
            else
            {
                const auto difference = std::find_if(
                    snapshot->configurationComparison.differences.begin(),
                    snapshot->configurationComparison.differences.end(),
                    [&options](const ScpConfigurationDifference& value) {
                        return value.quad ==
                                static_cast<int>(*options.quad) &&
                            value.hasRegister &&
                            value.registerOffset ==
                                *options.registerOffset;
                    });
                if (difference ==
                    snapshot->configurationComparison.differences.end())
                {
                    errorOutput <<
                        "error: the selected row is not a current banked "
                        "profile difference\n";
                }
                else if (FindFw2051ScpSetting(*options.registerOffset) ==
                         nullptr || difference->liveValue > 0xFFFFU ||
                         difference->profileValue > 0xFFFFU)
                {
                    errorOutput <<
                        "error: the selected difference is not an "
                        "applicable FW2051 D16 register\n";
                }
                else
                {
                    steps.push_back({
                        difference->quad,
                        *options.registerOffset,
                        difference->setting,
                        static_cast<std::uint16_t>(difference->liveValue),
                        static_cast<std::uint16_t>(difference->profileValue),
                        difference->displayHexadecimal,
                    });
                    readyToApply = true;
                }
            }
        }
    }

    if (readyToApply && !(interruptRequested && interruptRequested()))
    {
        output << "plan: writes=" << steps.size() << '\n';
        for (const auto& step : steps)
        {
            PrintApplicationStep(output, step);
        }
        output << "Apply " << steps.size()
               << " banked profile write(s) [y/N]: " << std::flush;
        std::string confirmation;
        if (!std::getline(input, confirmation) ||
            !TransactionConfirmedByUser(std::move(confirmation)))
        {
            output << "transaction: not applied\n";
        }
        else if (!(interruptRequested && interruptRequested()))
        {
            const auto beforeApply =
                tunerControl.CurrentSnapshot()->revision;
            if (options.command == CliCommand::ApplyAll)
            {
                tunerControl.Submit(ApplyAllDifferencesCommand{});
                if (!WaitForRevision(
                        tunerControl,
                        beforeApply,
                        [](const TunerSnapshot& value) {
                            return value.bulkApplyResult.state ==
                                    ScpBulkApplyState::Passed ||
                                value.bulkApplyResult.state ==
                                    ScpBulkApplyState::Failed;
                        }))
                {
                    errorOutput <<
                        "error: bulk SCP profile transaction timed out\n";
                }
                else
                {
                    snapshot = tunerControl.CurrentSnapshot();
                    PrintBulkApplyResult(output, snapshot->bulkApplyResult);
                    result = snapshot->bulkApplyResult.state ==
                                ScpBulkApplyState::Passed &&
                            snapshot->bulkApplyResult.profileValuesRetained &&
                            snapshot->bulkApplyResult.selectorParkedAtQuadZero
                        ? 0
                        : 1;
                }
            }
            else
            {
                tunerControl.Submit(ApplyProfileRowCommand{
                    *options.registerOffset,
                    *options.quad,
                });
                if (!WaitForRevision(
                        tunerControl,
                        beforeApply,
                        [](const TunerSnapshot& value) {
                            return value.singleRepairResult.state ==
                                    ScpSingleRepairState::Passed ||
                                value.singleRepairResult.state ==
                                    ScpSingleRepairState::Failed;
                        }))
                {
                    errorOutput <<
                        "error: single SCP profile transaction timed out\n";
                }
                else
                {
                    snapshot = tunerControl.CurrentSnapshot();
                    PrintSingleRepairResult(
                        output, snapshot->singleRepairResult);
                    result = snapshot->singleRepairResult.state ==
                                ScpSingleRepairState::Passed &&
                            snapshot->singleRepairResult.writeVerified &&
                            snapshot->singleRepairResult.profileValueRetained &&
                            snapshot->singleRepairResult
                                .selectorParkedAtQuadZero
                        ? 0
                        : 1;
                }
            }
            output << "stale_reminder: recapture all eight quads before "
                      "comparing or applying another value\n";
        }
    }

    return ReleaseOperationSession(
        tunerControl,
        output,
        errorOutput,
        interruptRequested,
        result);
}

int RunCliStartup(
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

    const auto beforeLoad = tunerControl.CurrentSnapshot()->revision;
    tunerControl.Submit(LoadProfileCommand{options.profilePath});
    if (!WaitForRevision(
            tunerControl,
            beforeLoad,
            [](const TunerSnapshot&) { return true; }))
    {
        errorOutput << "error: SCP profile load timed out\n";
        return 1;
    }
    auto snapshot = tunerControl.CurrentSnapshot();
    if (!snapshot->profileLoadedForTarget)
    {
        errorOutput << "error: SCP profile load failed";
        if (!snapshot->statusMessages.empty())
        {
            errorOutput << ": " << snapshot->statusMessages.back().summary;
        }
        errorOutput << '\n';
        return 1;
    }
    output << "profile_loaded: " << options.profilePath << '\n';
    if (interruptRequested && interruptRequested())
    {
        return 130;
    }

    const int openResult = ConfirmAndOpenSession(
        tunerControl, input, output, errorOutput, interruptRequested);
    if (openResult != 0)
    {
        return openResult;
    }

    int result = 1;
    bool recipeReady = false;
    if (!(interruptRequested && interruptRequested()))
    {
        const auto beforeAudit =
            tunerControl.CurrentSnapshot()->revision;
        tunerControl.Submit(RunStartupAuditCommand{});
        if (!WaitForRevision(
                tunerControl,
                beforeAudit,
                [](const TunerSnapshot& value) {
                    return value.startupAudit.state ==
                            StartupAuditState::Complete ||
                        value.startupAudit.state == StartupAuditState::Failed;
                }))
        {
            errorOutput << "error: startup audit timed out\n";
        }
        else
        {
            snapshot = tunerControl.CurrentSnapshot();
            if (snapshot->startupAudit.state != StartupAuditState::Complete)
            {
                errorOutput << "error: startup audit failed: "
                            << snapshot->startupAudit.message << '\n';
            }
        }
    }

    snapshot = tunerControl.CurrentSnapshot();
    if (snapshot->startupAudit.state == StartupAuditState::Complete &&
        !(interruptRequested && interruptRequested()))
    {
        const auto beforeCapture = snapshot->revision;
        tunerControl.Submit(CaptureConfigurationCommand{});
        if (!WaitForRevision(
                tunerControl,
                beforeCapture,
                [](const TunerSnapshot& value) {
                    return value.configurationCapture.state ==
                            ScpConfigurationState::Complete ||
                        value.configurationCapture.state ==
                            ScpConfigurationState::Failed;
                }))
        {
            errorOutput << "error: SCP configuration capture timed out\n";
        }
        else
        {
            snapshot = tunerControl.CurrentSnapshot();
            if (snapshot->configurationCapture.state !=
                ScpConfigurationState::Complete)
            {
                errorOutput << "error: SCP configuration capture failed: "
                            << snapshot->configurationCapture.message
                            << '\n';
            }
            else if (!snapshot->startupPlanAvailable ||
                     !snapshot->standaloneStartupPlan.success)
            {
                errorOutput << "error: deterministic startup is blocked: "
                            << snapshot->standaloneStartupPlan.message
                            << '\n';
            }
            else
            {
                PrintStartupRecipe(output, *snapshot);
                recipeReady = true;
            }
        }
    }

    if (recipeReady && !(interruptRequested && interruptRequested()))
    {
        const auto preparationCount =
            snapshot->startupPreparationMismatches.size();
        const auto bankedCount =
            snapshot->standaloneStartupPlan.bankedApplication.steps.size();
        output << "Run deterministic startup with " << preparationCount
               << " preparation change(s) and " << bankedCount
               << " banked write(s) [y/N]: " << std::flush;
        std::string confirmation;
        if (!std::getline(input, confirmation) ||
            !TransactionConfirmedByUser(std::move(confirmation)))
        {
            output << "startup: not run\n";
        }
        else if (!(interruptRequested && interruptRequested()))
        {
            const auto beforeStartup =
                tunerControl.CurrentSnapshot()->revision;
            tunerControl.Submit(RunDeterministicStartupCommand{true});
            if (!WaitForRevision(
                    tunerControl,
                    beforeStartup,
                    [](const TunerSnapshot& value) {
                        return value.deterministicStartupResult.state ==
                                DeterministicStartupState::Passed ||
                            value.deterministicStartupResult.state ==
                                DeterministicStartupState::Failed;
                    },
                    StartupCompletionTimeout))
            {
                errorOutput << "error: deterministic startup timed out\n";
            }
            else
            {
                snapshot = tunerControl.CurrentSnapshot();
                PrintDeterministicStartupResult(
                    output, snapshot->deterministicStartupResult);
                const auto& startup =
                    snapshot->deterministicStartupResult;
                result = startup.state == DeterministicStartupState::Passed &&
                        startup.finalProfileVerified &&
                        startup.finalComparison.valuesCompared ==
                            Fw2051ScpConfigurationValueCount &&
                        startup.finalComparison.differences.empty() &&
                        startup.moduleLeftStopped
                    ? 0
                    : 1;
            }
        }
    }

    return ReleaseOperationSession(
        tunerControl,
        output,
        errorOutput,
        interruptRequested,
        result);
}

int RunCliAcquire(
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

    auto snapshot = tunerControl.CurrentSnapshot();
    const std::string profilePath = !options.profilePath.empty()
        ? options.profilePath
        : snapshot->activeModuleProfilePath;
    if (profilePath.empty())
    {
        errorOutput << "error: the selected module has no profile path\n";
        return 1;
    }
    const auto beforeLoad = snapshot->revision;
    tunerControl.Submit(LoadProfileCommand{profilePath});
    if (!WaitForRevision(
            tunerControl,
            beforeLoad,
            [](const TunerSnapshot&) { return true; }))
    {
        errorOutput << "error: SCP profile load timed out\n";
        return 1;
    }
    snapshot = tunerControl.CurrentSnapshot();
    if (!snapshot->profileLoadedForTarget)
    {
        errorOutput << "error: SCP profile load failed\n";
        return 1;
    }
    output << "profile_loaded: " << profilePath << '\n';

    const int openResult = ConfirmAndOpenSession(
        tunerControl, input, output, errorOutput, interruptRequested);
    if (openResult != 0)
    {
        return openResult;
    }

    int result = 1;
    bool startupReady = false;
    if (!(interruptRequested && interruptRequested()))
    {
        const auto beforeAudit = tunerControl.CurrentSnapshot()->revision;
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
    }
    snapshot = tunerControl.CurrentSnapshot();
    if (snapshot->startupAudit.state == StartupAuditState::Complete
        && !(interruptRequested && interruptRequested()))
    {
        const auto beforeCapture = snapshot->revision;
        tunerControl.Submit(CaptureConfigurationCommand{});
        if (!WaitForRevision(
                tunerControl,
                beforeCapture,
                [](const TunerSnapshot& value) {
                    return value.configurationCapture.state
                            == ScpConfigurationState::Complete
                        || value.configurationCapture.state
                            == ScpConfigurationState::Failed;
                }))
        {
            errorOutput << "error: SCP configuration capture timed out\n";
        }
        else
        {
            snapshot = tunerControl.CurrentSnapshot();
            if (!snapshot->startupPlanAvailable
                || !snapshot->standaloneStartupPlan.success)
            {
                errorOutput << "error: deterministic startup is blocked: "
                            << snapshot->standaloneStartupPlan.message
                            << '\n';
            }
            else
            {
                PrintStartupRecipe(output, *snapshot);
                startupReady = true;
            }
        }
    }

    if (startupReady && !(interruptRequested && interruptRequested()))
    {
        const auto preparationCount =
            snapshot->startupPreparationMismatches.size();
        const auto bankedCount =
            snapshot->standaloneStartupPlan.bankedApplication.steps.size();
        output << "Run deterministic startup with " << preparationCount
               << " preparation change(s) and " << bankedCount
               << " banked write(s) before acquisition [y/N]: "
               << std::flush;
        std::string confirmation;
        if (!std::getline(input, confirmation)
            || !TransactionConfirmedByUser(std::move(confirmation)))
        {
            output << "acquisition: not started\n";
        }
        else
        {
            const auto beforeStartup =
                tunerControl.CurrentSnapshot()->revision;
            tunerControl.Submit(RunDeterministicStartupCommand{true});
            if (!WaitForRevision(
                    tunerControl,
                    beforeStartup,
                    [](const TunerSnapshot& value) {
                        return value.deterministicStartupResult.state
                                == DeterministicStartupState::Passed
                            || value.deterministicStartupResult.state
                                == DeterministicStartupState::Failed;
                    },
                    StartupCompletionTimeout))
            {
                errorOutput << "error: deterministic startup timed out\n";
            }
            else
            {
                snapshot = tunerControl.CurrentSnapshot();
                PrintDeterministicStartupResult(
                    output, snapshot->deterministicStartupResult);
            }
        }
    }

    snapshot = tunerControl.CurrentSnapshot();
    if (snapshot->deterministicStartupPassed
        && !(interruptRequested && interruptRequested()))
    {
        const auto beforeStart = snapshot->revision;
        tunerControl.Submit(StartDiagnosticAcquisitionCommand{
            *options.channel,
        });
        if (!WaitForRevision(
                tunerControl,
                beforeStart,
                [](const TunerSnapshot& value) {
                    return value.acquisition
                            == GuidedTunerAcquisitionState::Running
                        || value.acquisition
                            == GuidedTunerAcquisitionState::Failed;
                },
                AcquisitionCompletionTimeout))
        {
            errorOutput << "error: acquisition start timed out\n";
        }
        else
        {
            snapshot = tunerControl.CurrentSnapshot();
            if (snapshot->acquisition == GuidedTunerAcquisitionState::Running)
            {
                result = 0;
                output << "acquisition: running channel="
                       << *options.channel << '\n';
                PrintAcquisitionIsolation(
                    output, snapshot->diagnosticAcquisition);
                output << "acquisition_commands: s0-s3 source | "
                          "p <register> <value> preview | r restore | "
                          "Enter stop\n";
                std::uint32_t elapsedIntervals = 0U;
                while (!(interruptRequested && interruptRequested()))
                {
                    snapshot = tunerControl.CurrentSnapshot();
                    PrintAcquisitionStatus(output, *snapshot);
                    if (snapshot->acquisition
                        != GuidedTunerAcquisitionState::Running)
                    {
                        result = 1;
                        break;
                    }
                    if (options.seconds
                        && elapsedIntervals >= *options.seconds)
                    {
                        break;
                    }

                    const auto waited = waitForInput();
                    if (waited == CliSessionWaitResult::OneSecondElapsed)
                    {
                        ++elapsedIntervals;
                        continue;
                    }
                    if (waited == CliSessionWaitResult::Interrupted)
                    {
                        continue;
                    }
                    if (waited == CliSessionWaitResult::InputReady)
                    {
                        std::string commandLine;
                        if (!std::getline(input, commandLine)
                            || commandLine.empty())
                        {
                            break;
                        }
                        if (!ParseAcquisitionCommand(
                                commandLine,
                                tunerControl,
                                output,
                                errorOutput))
                        {
                            result = 1;
                            break;
                        }
                        snapshot = tunerControl.CurrentSnapshot();
                        if (snapshot->acquisition
                            != GuidedTunerAcquisitionState::Running)
                        {
                            result = 1;
                            break;
                        }
                        continue;
                    }
                    errorOutput << "error: acquisition input wait failed\n";
                    result = 1;
                    break;
                }

                snapshot = tunerControl.CurrentSnapshot();
                if (snapshot->acquisition
                    == GuidedTunerAcquisitionState::Running)
                {
                    const auto beforeStop = snapshot->revision;
                    tunerControl.Submit(StopDiagnosticAcquisitionCommand{});
                    if (!WaitForRevision(
                            tunerControl,
                            beforeStop,
                            [](const TunerSnapshot& value) {
                                return value.acquisition
                                        == GuidedTunerAcquisitionState::Stopped
                                    || value.acquisition
                                        == GuidedTunerAcquisitionState::Failed;
                            },
                            AcquisitionCompletionTimeout))
                    {
                        errorOutput << "error: verified acquisition cleanup "
                                       "timed out\n";
                        result = 1;
                    }
                    snapshot = tunerControl.CurrentSnapshot();
                }
                PrintCleanupResult(
                    output, snapshot->diagnosticAcquisition);
                if (!snapshot->cleanupVerified)
                {
                    result = 1;
                }
            }
            else
            {
                errorOutput << "error: acquisition start failed: "
                            << snapshot->diagnosticAcquisition.message
                            << '\n';
            }
        }
    }

    snapshot = tunerControl.CurrentSnapshot();
    if (result == 0 && !options.dumpCsvPath.empty())
    {
        std::string csvError;
        if (!DumpRequestedWaveformCsv(
                options.dumpCsvPath,
                snapshot->diagnosticStream,
                csvError))
        {
            errorOutput << "error: CSV dump failed: " << csvError << '\n';
            result = 1;
        }
        else
        {
            output << "waveform_csv: " << options.dumpCsvPath << '\n';
        }
    }

    return ReleaseOperationSession(
        tunerControl,
        output,
        errorOutput,
        interruptRequested,
        result);
}

} // namespace fidget
