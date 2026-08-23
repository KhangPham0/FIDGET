#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "cli/CliApp.h"
#include "core/ScpProfile.h"
#include "core/ScpRegistry.h"
#include "core/StartupAudit.h"
#include "core/StartupPreparation.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

struct TemporaryCsv
{
    std::string path;

    TemporaryCsv()
    {
        const auto unique = std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count();
        path = "/tmp/fidget_cli_waveform_" + std::to_string(unique)
            + ".csv";
    }

    ~TemporaryCsv()
    {
        std::remove(path.c_str());
    }

    TemporaryCsv(const TemporaryCsv&) = delete;
    TemporaryCsv& operator=(const TemporaryCsv&) = delete;
};

struct TemporaryMvmeExport
{
    std::string profilePath;
    std::string outputPath;

    TemporaryMvmeExport()
    {
        const auto unique = std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count();
        const std::string stem = "/tmp/fidget_cli_export_"
            + std::to_string(unique);
        profilePath = stem + ".mwwscp";
        outputPath = stem + ".mvme";
    }

    ~TemporaryMvmeExport()
    {
        std::remove(profilePath.c_str());
        std::remove(outputPath.c_str());
        std::remove((outputPath + ".tmp").c_str());
    }

    TemporaryMvmeExport(const TemporaryMvmeExport&) = delete;
    TemporaryMvmeExport& operator=(const TemporaryMvmeExport&) = delete;
};

struct TemporaryCrateProject
{
    std::string path;

    explicit TemporaryCrateProject(bool sshBridge = false)
    {
        const auto unique = std::chrono::steady_clock::now()
                                .time_since_epoch()
                                .count();
        path = "/tmp/fidget_cli_recovery_" + std::to_string(unique)
            + ".mwwcrate";
        fidget::CrateProject project;
        project.mvlcHost = "mvlc-test";
        project.mvlcCommandPort = 32768U;
        project.streamHost = "stream-test";
        project.streamPort = 42333U;
        if (sshBridge)
        {
            project.endpointKind =
                fidget::CrateProjectEndpointKind::SshBridge;
            project.sshDestination = "daq-through-bastion";
            project.remoteBridgeCommand =
                "/opt/fidget/bin/fidget_bridge";
        }
        project.modules.push_back({
            "MDPP-32 SCP",
            0x11000000U,
            fidget::MdppBackend::Scp,
            "mdpp1_scp_profile.mwwscp",
        });
        REQUIRE(fidget::SaveCrateProject(project, path).success);
    }

    ~TemporaryCrateProject()
    {
        std::remove(path.c_str());
        std::remove((path + ".activity").c_str());
    }

    TemporaryCrateProject(const TemporaryCrateProject&) = delete;
    TemporaryCrateProject& operator=(const TemporaryCrateProject&) = delete;
};

fidget::TunerRecoveryRecord MakeRecoveryRecord()
{
    fidget::TunerRecoveryRecord record;
    record.phase = fidget::TunerRecoveryPhase::Active;
    record.host = "mvlc-test";
    record.commandPort = 32768U;
    record.mvlcHardwareId = 0x5008U;
    record.mvlcFirmwareRevision = 0x0046U;
    record.mdppBaseAddress = 0x11000000U;
    record.mdppHardwareId = 0x5007U;
    record.mdppIrqLevel = 3U;
    record.mdppOutputFormat = 0x0018U;
    record.stackTriggerRegister = 0x1104U;
    record.stackTriggerValue = 0x0042U;
    record.stackOffsetRegister = 0x1204U;
    record.stackOffsetValue = 0x0200U;
    record.ownershipTokenRegister = 0x221CU;
    record.ownershipTokenValue = 0xA55A1234U;
    record.previewRestoreRequired = true;
    record.previewQuad = 7U;
    record.previewRegisterOffset = 0x611AU;
    record.previewOriginalValue = 200U;
    record.previewAppliedValue = 250U;
    record.sourceRestoreRequired = true;
    record.sourceQuad = 7U;
    record.sourceOriginalConfiguration = 0x0040U;
    record.sourceAppliedConfigurationAvailable = true;
    record.sourceAppliedConfiguration = 0x0043U;
    return record;
}

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
            projectEndpointKind = project->project.endpointKind;
            projectSshDestination = project->project.sshDestination;
            projectRemoteBridgeCommand =
                project->project.remoteBridgeCommand;
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
            next.projectPath = project->projectPath;
            next.recoveryJournalPath = project->projectPath + ".recovery";
            next.recoveryJournalStatus = recoveryJournalStatus;
            next.recoveryRecordAvailable = recoveryJournalStatus
                != fidget::RecoveryJournalStatus::None;
            if (recoveryJournalStatus
                == fidget::RecoveryJournalStatus::Pending)
            {
                next.recoveryRecord = MakeRecoveryRecord();
                next.ownership =
                    fidget::GuidedTunerOwnershipState::RecoveryRequired;
            }
            else if (recoveryJournalStatus
                     == fidget::RecoveryJournalStatus::Malformed)
            {
                next.recoveryJournalMessage =
                    "Tuner recovery-journal checksum mismatch.";
                next.ownership =
                    fidget::GuidedTunerOwnershipState::RecoveryRequired;
            }
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
                "Saved the SCP profile.",
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
            next.singleRepairResult.moduleStopSent = true;
            next.singleRepairResult.moduleStopVerified = true;
            next.singleRepairResult.writeAttempted = true;
            next.singleRepairResult.writeVerified = true;
            next.singleRepairResult.selectorParkedAtQuadZero = true;
            next.singleRepairResult.fifoResetSent = true;
            next.singleRepairResult.readoutResetSent = true;
            next.singleRepairResult.moduleLeftStopped = true;
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
        else if (const auto* start = std::get_if<
                     fidget::StartDiagnosticAcquisitionCommand>(&command))
        {
            ++startAcquisitionCommands;
            next.acquisition = fidget::GuidedTunerAcquisitionState::Running;
            next.diagnosticAcquisition.state =
                fidget::DiagnosticAcquisitionState::Running;
            next.diagnosticAcquisition.requestedChannel = start->channel;
            next.diagnosticAcquisition.message =
                "Direct diagnostic acquisition is running.";
            next.diagnosticAcquisition.nonTargetModuleCount = 1U;
            next.diagnosticAcquisition.nonTargetModulesQuiesced = 1U;
            fidget::DiagnosticModuleIsolation isolation;
            isolation.baseAddress = 0x22000000U;
            isolation.hardwareId = 0x5007U;
            isolation.irqLevel = 0U;
            isolation.acquisitionStateBefore = 1U;
            isolation.stopVerified = true;
            isolation.fifoResetSent = true;
            isolation.readoutResetSent = true;
            next.diagnosticAcquisition.moduleIsolation = {isolation};
            next.diagnosticStream.receiverRunning = true;
            next.diagnosticStream.requestedChannel = start->channel;
            next.diagnosticStream.requestedTarget = {
                0x11,
                static_cast<int>(start->channel),
                true,
                true,
            };
            next.diagnosticStream.datagramsReceived = 12U;
            next.diagnosticStream.bytesReceived = 480U;
            next.diagnosticStream.decoderStats.ethernetPackets = 12U;
            if (!acquisitionSilent)
            {
                next.diagnosticStream.decoderStats.decodedWaveforms = 4U;
                next.diagnosticStream.channelWaveformTotals = {
                    {0U, 2U},
                    {3U, 1U},
                    {start->channel, 4U},
                };
                fidget::MdppWaveform waveform;
                waveform.sequence = 4U;
                waveform.moduleId = 0x11U;
                waveform.channel = start->channel;
                waveform.samples = {-2, 10, 100, -100};
                fidget::MdppChannelHistorySnapshot history;
                history.totalCaptured = 4U;
                history.waveforms.push_back(std::move(waveform));
                next.diagnosticStream.histories.emplace(
                    fidget::MdppChannelAddress{
                        0x11U,
                        static_cast<int>(start->channel),
                    },
                    std::move(history));
            }
        }
        else if (std::holds_alternative<
                     fidget::StopDiagnosticAcquisitionCommand>(command))
        {
            ++stopAcquisitionCommands;
            next.acquisition = fidget::GuidedTunerAcquisitionState::Stopped;
            next.cleanupVerified = true;
            next.diagnosticAcquisition.state =
                fidget::DiagnosticAcquisitionState::Stopped;
            next.diagnosticAcquisition.message =
                "Direct acquisition stopped cleanly.";
            next.diagnosticAcquisition.moduleStopSent = true;
            next.diagnosticAcquisition.daqModeDisabled = true;
            next.diagnosticAcquisition.readoutStackDisabled = true;
            next.diagnosticAcquisition.recoveryJournalRemoved = true;
            next.diagnosticAcquisition.previewRestoreAttemptedOnStop =
                next.diagnosticParameterPreview.previewActive;
            next.diagnosticAcquisition.previewRestoreVerifiedOnStop =
                next.diagnosticParameterPreview.previewActive;
            next.diagnosticAcquisition.sourceRestoreAttemptedOnStop =
                next.diagnosticSourceChange.sourceRestoreRequired;
            next.diagnosticAcquisition.sourceRestoreVerifiedOnStop =
                next.diagnosticSourceChange.sourceRestoreRequired;
            next.diagnosticStream.receiverRunning = false;
        }
        else if (const auto* source = std::get_if<
                     fidget::ChangeDiagnosticSourceCommand>(&command))
        {
            ++sourceChangeCommands;
            next.diagnosticSourceChange = {};
            next.diagnosticSourceChange.state =
                fidget::DiagnosticSourceChangeState::Passed;
            next.diagnosticSourceChange.message =
                "Waveform source applied and acquisition resumed.";
            next.diagnosticSourceChange.selectedQuad =
                static_cast<std::uint16_t>(
                    next.diagnosticAcquisition.requestedChannel / 4U);
            next.diagnosticSourceChange.requestedSource = source->source;
            next.diagnosticSourceChange.originalConfiguration = 0x00C1U;
            next.diagnosticSourceChange.requestedConfiguration =
                static_cast<std::uint16_t>(0x00C0U | source->source);
            next.diagnosticSourceChange.appliedReadback =
                next.diagnosticSourceChange.requestedConfiguration;
            next.diagnosticSourceChange.writeVerified = true;
            next.diagnosticSourceChange.sourceRestoreRequired = true;
            next.diagnosticSourceChange.selectorParkedAtQuadZero = true;
            next.diagnosticSourceChange.acquisitionResumed = true;
            next.diagnosticSourceChange.daqModeResumed = true;
        }
        else if (const auto* preview = std::get_if<
                     fidget::ApplyDiagnosticPreviewCommand>(&command))
        {
            ++previewCommands;
            next.diagnosticParameterPreview = {};
            next.diagnosticParameterPreview.state =
                fidget::DiagnosticParameterPreviewState::PreviewActive;
            next.diagnosticParameterPreview.message =
                "Gain preview applied and verified; acquisition resumed.";
            next.diagnosticParameterPreview.selectedQuad =
                static_cast<std::uint16_t>(
                    next.diagnosticAcquisition.requestedChannel / 4U);
            next.diagnosticParameterPreview.registerOffset =
                preview->registerOffset;
            next.diagnosticParameterPreview.settingName = "Gain";
            next.diagnosticParameterPreview.originalValue = 200U;
            next.diagnosticParameterPreview.requestedValue = preview->value;
            next.diagnosticParameterPreview.appliedReadback = preview->value;
            next.diagnosticParameterPreview.applyDurationMicroseconds = 75U;
            next.diagnosticParameterPreview.originalCaptured = true;
            next.diagnosticParameterPreview.writeVerified = true;
            next.diagnosticParameterPreview.previewActive = true;
        }
        else if (std::holds_alternative<
                     fidget::RestoreDiagnosticPreviewCommand>(command))
        {
            ++restorePreviewCommands;
            auto& preview = next.diagnosticParameterPreview;
            preview.state =
                fidget::DiagnosticParameterPreviewState::Restored;
            preview.message = "Original Gain restored and verified.";
            preview.restoredReadback = preview.originalValue;
            preview.restoreDurationMicroseconds = 80U;
            preview.previewActive = false;
            preview.restoreVerified = true;
        }
        else if (const auto* recovery = std::get_if<
                     fidget::RecoverDiagnosticOrphanCommand>(&command))
        {
            if (!recovery->confirmed)
            {
                ++recoveryStatusCommands;
                next.ownership =
                    fidget::GuidedTunerOwnershipState::RecoveryRequired;
                next.controllerReadingsValid = recoveryStatusSucceeds;
                next.mvlcHardwareId = 0x5008U;
                next.mvlcFirmwareRevision = 0x0046U;
                next.mvlcDaqMode = 0x00000005U;
                next.statusMessages = {{
                    recoveryStatusSucceeds
                        ? fidget::TunerStatusLevel::Warning
                        : fidget::TunerStatusLevel::Error,
                    recoveryStatusSucceeds
                        ? "Only fingerprint-gated orphan recovery is allowed."
                        : "Recovery status failed.",
                    {},
                }};
            }
            else
            {
                ++recoveryCommands;
                next.diagnosticRecovery = {};
                next.diagnosticRecovery.state = recoveryOutcome;
                next.diagnosticRecovery.message = recoveryOutcome
                        == fidget::DiagnosticOrphanRecoveryState::Recovered
                    ? "Recovered the tuner-owned diagnostic orphan."
                    : recoveryOutcome
                            == fidget::DiagnosticOrphanRecoveryState::AlreadyClean
                        ? "The journal was stale and was removed."
                        : "Unique tuner ownership token mismatched.";
                next.diagnosticRecovery.steps.push_back({
                    "fingerprint",
                    recoveryOutcome
                        != fidget::DiagnosticOrphanRecoveryState::ForeignOrMismatched,
                    "Checked the complete unique tuner fingerprint.",
                });
                if (recoveryOutcome
                        == fidget::DiagnosticOrphanRecoveryState::Recovered
                    || recoveryOutcome
                        == fidget::DiagnosticOrphanRecoveryState::AlreadyClean)
                {
                    next.recoveryJournalStatus =
                        fidget::RecoveryJournalStatus::None;
                    next.recoveryRecordAvailable = false;
                    next.recoveryRecord.reset();
                    next.ownership =
                        fidget::GuidedTunerOwnershipState::Disconnected;
                }
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
    int startAcquisitionCommands = 0;
    int stopAcquisitionCommands = 0;
    int sourceChangeCommands = 0;
    int previewCommands = 0;
    int restorePreviewCommands = 0;
    int recoveryStatusCommands = 0;
    int recoveryCommands = 0;
    int releaseCommands = 0;
    fidget::CrateProjectEndpointKind projectEndpointKind =
        fidget::CrateProjectEndpointKind::Direct;
    std::string projectSshDestination;
    std::string projectRemoteBridgeCommand;
    fidget::RecoveryJournalStatus recoveryJournalStatus =
        fidget::RecoveryJournalStatus::None;
    fidget::DiagnosticOrphanRecoveryState recoveryOutcome =
        fidget::DiagnosticOrphanRecoveryState::Recovered;
    bool recoveryStatusSucceeds = true;
    bool acquisitionSilent = false;
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
    {
        const char* arguments[] = {
            "fidget_cli", "acquire", "--project", "crate.mwwcrate",
            "--profile", "expected.mwwscp", "--channel", "29",
            "--seconds", "30", "--dump-csv", "waveform.csv",
        };
        const auto parsed = fidget::ParseCliOptions(12, arguments);
        REQUIRE(parsed.success);
        CHECK(parsed.options.command == fidget::CliCommand::Acquire);
        REQUIRE(parsed.options.channel);
        CHECK(*parsed.options.channel == 29U);
        REQUIRE(parsed.options.seconds);
        CHECK(*parsed.options.seconds == 30U);
        CHECK(parsed.options.dumpCsvPath == "waveform.csv");
    }
    {
        const char* arguments[] = {
            "fidget_cli", "export", "--profile", "expected.mwwscp",
            "--out", "expected.mvme",
        };
        const auto parsed = fidget::ParseCliOptions(6, arguments);
        REQUIRE(parsed.success);
        CHECK(parsed.options.command == fidget::CliCommand::Export);
        CHECK(parsed.options.profilePath == "expected.mwwscp");
        CHECK(parsed.options.outputPath == "expected.mvme");
        CHECK(parsed.options.projectPath.empty());
        CHECK(parsed.options.host.empty());
    }
}

TEST_CASE("acquisition requires a project-backed recovery path")
{
    const char* arguments[] = {
        "fidget_cli", "acquire", "--host", "mvlc-test",
        "--profile", "expected.mwwscp", "--channel", "29",
    };
    const auto parsed = fidget::ParseCliOptions(8, arguments);
    CHECK_FALSE(parsed.success);
    CHECK(parsed.error
          == "acquire requires --project FILE for recovery journaling");
}

TEST_CASE("CLI options accept recovery only with a crate project")
{
    const char* valid[] = {
        "fidget_cli", "recover", "--project", "crate.mwwcrate",
        "--module", "2",
    };
    const auto parsed = fidget::ParseCliOptions(6, valid);
    REQUIRE(parsed.success);
    CHECK(parsed.options.command == fidget::CliCommand::Recover);
    CHECK(parsed.options.projectPath == "crate.mwwcrate");
    CHECK(parsed.options.moduleIndex == 1U);

    const char* hostOnly[] = {
        "fidget_cli", "recover", "--host", "mvlc-test",
    };
    const auto refused = fidget::ParseCliOptions(4, hostOnly);
    CHECK_FALSE(refused.success);
    CHECK(refused.error == "recover requires --project FILE");
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

    const char* acquireWithoutChannel[] = {
        "fidget_cli", "acquire", "--host", "mvlc-test",
    };
    CHECK_FALSE(
        fidget::ParseCliOptions(4, acquireWithoutChannel).success);

    const char* invalidChannel[] = {
        "fidget_cli", "acquire", "--host", "mvlc-test",
        "--channel", "32",
    };
    CHECK_FALSE(fidget::ParseCliOptions(6, invalidChannel).success);

    const char* exportWithoutProfile[] = {
        "fidget_cli", "export", "--out", "expected.mvme",
    };
    CHECK_FALSE(
        fidget::ParseCliOptions(4, exportWithoutProfile).success);

    const char* exportWithoutOutput[] = {
        "fidget_cli", "export", "--profile", "expected.mwwscp",
    };
    CHECK_FALSE(
        fidget::ParseCliOptions(4, exportWithoutOutput).success);

    const char* outputOnStatus[] = {
        "fidget_cli", "status", "--host", "mvlc-test",
        "--out", "unexpected.mvme",
    };
    CHECK_FALSE(fidget::ParseCliOptions(6, outputOnStatus).success);
}

TEST_CASE("offline export writes a manifest and activity entry")
{
    TemporaryMvmeExport files;
    auto configuration = MakeConfiguration(false);
    configuration.selectorWrites.clear();
    REQUIRE(fidget::SaveFw2051ScpProfile(
                configuration, files.profilePath)
                .success);

    fidget::CliOptions options;
    options.command = fidget::CliCommand::Export;
    options.profilePath = files.profilePath;
    options.outputPath = files.outputPath;
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliExport(
        options, input, output, errors);

    CHECK(exitCode == 0);
    CHECK(errors.str().empty());
    CHECK(output.str().find("export_manifest: checksum=")
          != std::string::npos);
    CHECK(output.str().find(" values=141 ") != std::string::npos);
    CHECK(output.str().find("export_output: " + files.outputPath)
          != std::string::npos);
    CHECK(output.str().find("activity: ") != std::string::npos);
    CHECK(output.str().find("[export] [success]") != std::string::npos);

    std::ifstream saved(files.outputPath, std::ios::binary);
    REQUIRE(saved.good());
    const std::string text{
        std::istreambuf_iterator<char>(saved),
        std::istreambuf_iterator<char>()};
    CHECK(text.find("# ===== BEGIN FIDGET MVME EXPORT =====\n") == 0U);
    CHECK(text.find("# value_count: 141\n") != std::string::npos);
}

TEST_CASE("offline export refuses replacement until typed y")
{
    TemporaryMvmeExport files;
    auto configuration = MakeConfiguration(false);
    configuration.selectorWrites.clear();
    REQUIRE(fidget::SaveFw2051ScpProfile(
                configuration, files.profilePath)
                .success);
    {
        std::ofstream existing(files.outputPath, std::ios::binary);
        existing << "keep me\n";
    }

    fidget::CliOptions options;
    options.command = fidget::CliCommand::Export;
    options.profilePath = files.profilePath;
    options.outputPath = files.outputPath;
    std::istringstream refusedInput;
    std::ostringstream refusedOutput;
    std::ostringstream refusedErrors;

    CHECK(fidget::RunCliExport(
              options,
              refusedInput,
              refusedOutput,
              refusedErrors)
          == 1);
    std::ifstream unchanged(files.outputPath, std::ios::binary);
    std::string firstLine;
    std::getline(unchanged, firstLine);
    CHECK(firstLine == "keep me");

    std::istringstream confirmedInput("y\n");
    std::ostringstream confirmedOutput;
    std::ostringstream confirmedErrors;
    CHECK(fidget::RunCliExport(
              options,
              confirmedInput,
              confirmedOutput,
              confirmedErrors)
          == 0);
    std::ifstream replaced(files.outputPath, std::ios::binary);
    std::getline(replaced, firstLine);
    CHECK(firstLine == "# ===== BEGIN FIDGET MVME EXPORT =====");
}

TEST_CASE("offline export reports a missing profile without output")
{
    TemporaryMvmeExport files;
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Export;
    options.profilePath = files.profilePath;
    options.outputPath = files.outputPath;
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream errors;

    CHECK(fidget::RunCliExport(options, input, output, errors) == 1);
    CHECK(errors.str().find("SCP profile load failed")
          != std::string::npos);
    std::ifstream absent(files.outputPath, std::ios::binary);
    CHECK_FALSE(absent.good());
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

TEST_CASE("status preserves an SSH bridge endpoint loaded from a project")
{
    TemporaryCrateProject project(true);
    fidget::CliOptions options;
    options.projectPath = project.path;
    FakeTunerControl control;
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliStatus(
        options, control, output, errors, [] { return false; });

    CHECK(exitCode == 0);
    CHECK(control.projectCommands == 1);
    CHECK(control.projectEndpointKind
          == fidget::CrateProjectEndpointKind::SshBridge);
    CHECK(control.projectSshDestination == "daq-through-bastion");
    CHECK(control.projectRemoteBridgeCommand
          == "/opt/fidget/bin/fidget_bridge");
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
              "rollback=none retained=profile stop=verified fifo_reset=yes "
              "readout_reset=yes final_stopped=verified parked=yes\n") !=
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
              "Apply 1 banked profile write(s) and leave the module stopped "
              "[y/N]: ") !=
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

TEST_CASE("acquire runs startup streams status and verifies cleanup")
{
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Acquire;
    options.host = "mvlc-test";
    options.profilePath = "expected.mwwscp";
    options.channel = 29U;
    options.seconds = 1U;
    FakeTunerControl control(
        fidget::GuidedTunerOwnershipState::Idle, false, true);
    std::istringstream input("yes\ny\n");
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliAcquire(
        options,
        control,
        input,
        output,
        errors,
        [] { return false; },
        [] { return fidget::CliSessionWaitResult::OneSecondElapsed; });

    CHECK(exitCode == 0);
    CHECK(control.auditCommands == 1);
    CHECK(control.captureCommands == 1);
    CHECK(control.startupCommands == 1);
    CHECK(control.startAcquisitionCommands == 1);
    CHECK(control.stopAcquisitionCommands == 1);
    CHECK(control.releaseCommands == 1);
    CHECK(errors.str().empty());
    CHECK(output.str().find("acquisition: running channel=29\n")
          != std::string::npos);
    CHECK(output.str().find(
              "acquisition_isolation: checked=1 quiesced=1\n")
          != std::string::npos);
    CHECK(output.str().find(
              "isolation_start: base=0x22000000 hardware=0x5007 "
              "irq=0 state_before=1 quiesced=yes stop=verified "
              "fifo_reset=yes readout_reset=yes\n")
          != std::string::npos);
    CHECK(output.str().find(
              "packets=12 datagrams=12 waveforms=4 loss=0 "
              "decode_errors=0 channel=29 channel_count=4 "
              "fingerprint=matching")
          != std::string::npos);
    CHECK(output.str().find(
              "acquisition_channels: 0=2 3=1 29=4\n")
          != std::string::npos);
    CHECK(output.str().find(
              "cleanup_selected: stopped=yes\n") != std::string::npos);
    CHECK(output.str().find(
              "cleanup_mvlc: daq_mode_zero=yes stack_zero=yes "
              "journal_removed=yes\n") != std::string::npos);
    CHECK(output.str().find("session: released\n") != std::string::npos);
}

TEST_CASE("acquire dumps the latest requested-channel waveform as CSV")
{
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Acquire;
    options.host = "mvlc-test";
    options.profilePath = "expected.mwwscp";
    options.channel = 29U;
    options.seconds = 1U;
    const TemporaryCsv csv;
    options.dumpCsvPath = csv.path;
    FakeTunerControl control(
        fidget::GuidedTunerOwnershipState::Idle, false, true);
    std::istringstream input("yes\ny\n");
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliAcquire(
        options,
        control,
        input,
        output,
        errors,
        [] { return false; },
        [] { return fidget::CliSessionWaitResult::OneSecondElapsed; });

    REQUIRE(exitCode == 0);
    std::ifstream saved(csv.path);
    REQUIRE(saved.good());
    std::ostringstream contents;
    contents << saved.rdbuf();
    CHECK(contents.str()
          == "sample_index,value\n0,-2\n1,10\n2,100\n3,-100\n");
    CHECK(output.str().find("waveform_csv: " + csv.path + '\n')
          != std::string::npos);
}

TEST_CASE("acquire accepts source preview restore and Enter commands")
{
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Acquire;
    options.host = "mvlc-test";
    options.profilePath = "expected.mwwscp";
    options.channel = 29U;
    FakeTunerControl control(
        fidget::GuidedTunerOwnershipState::Idle, false, true);
    std::istringstream input("yes\ny\ns2\np 0x611A 250\nr\n\n");
    std::ostringstream output;
    std::ostringstream errors;
    std::size_t inputWaits = 0U;

    const int exitCode = fidget::RunCliAcquire(
        options,
        control,
        input,
        output,
        errors,
        [] { return false; },
        [&inputWaits] {
            ++inputWaits;
            return fidget::CliSessionWaitResult::InputReady;
        });

    CHECK(exitCode == 0);
    CHECK(inputWaits == 4U);
    CHECK(control.sourceChangeCommands == 1);
    CHECK(control.previewCommands == 1);
    CHECK(control.restorePreviewCommands == 1);
    CHECK(control.stopAcquisitionCommands == 1);
    CHECK(control.releaseCommands == 1);
    CHECK(errors.str().empty());
    CHECK(output.str().find(
              "source_change: state=passed quad=7 source=2")
          != std::string::npos);
    CHECK(output.str().find(
              "parameter_preview: state=active quad=7 register=0x611A")
          != std::string::npos);
    CHECK(output.str().find(
              "parameter_preview: state=restored quad=7 register=0x611A")
          != std::string::npos);
}

TEST_CASE("invalid acquire mini command keeps monitoring without hardware")
{
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Acquire;
    options.host = "mvlc-test";
    options.profilePath = "expected.mwwscp";
    options.channel = 29U;
    FakeTunerControl control(
        fidget::GuidedTunerOwnershipState::Idle, false, true);
    std::istringstream input("yes\ny\np 0x6148 399\n\n");
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliAcquire(
        options,
        control,
        input,
        output,
        errors,
        [] { return false; },
        [] { return fidget::CliSessionWaitResult::InputReady; });

    CHECK(exitCode == 0);
    CHECK(control.previewCommands == 0);
    CHECK(control.stopAcquisitionCommands == 1);
    CHECK(errors.str().find("must be an even number of samples")
          != std::string::npos);
}

TEST_CASE("SIGINT after a tune command still stops and releases in order")
{
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Acquire;
    options.host = "mvlc-test";
    options.profilePath = "expected.mwwscp";
    options.channel = 29U;
    FakeTunerControl control(
        fidget::GuidedTunerOwnershipState::Idle, false, true);
    std::istringstream input("yes\ny\ns3\n");
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliAcquire(
        options,
        control,
        input,
        output,
        errors,
        [&control] { return control.sourceChangeCommands > 0; },
        [] { return fidget::CliSessionWaitResult::InputReady; });

    CHECK(exitCode == 130);
    CHECK(control.sourceChangeCommands == 1);
    CHECK(control.stopAcquisitionCommands == 1);
    CHECK(control.releaseCommands == 1);
    CHECK(output.str().find("session: released\n") != std::string::npos);
}

TEST_CASE("acquire defaults to no before startup and releases")
{
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Acquire;
    options.host = "mvlc-test";
    options.profilePath = "expected.mwwscp";
    options.channel = 29U;
    FakeTunerControl control(
        fidget::GuidedTunerOwnershipState::Idle, false, true);
    std::istringstream input("yes\n");
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliAcquire(
        options,
        control,
        input,
        output,
        errors,
        [] { return false; },
        [] { return fidget::CliSessionWaitResult::InputReady; });

    CHECK(exitCode == 1);
    CHECK(control.startupCommands == 0);
    CHECK(control.startAcquisitionCommands == 0);
    CHECK(control.stopAcquisitionCommands == 0);
    CHECK(control.releaseCommands == 1);
    CHECK(output.str().find("acquisition: not started\n")
          != std::string::npos);
}

TEST_CASE("SIGINT during acquire stops and releases before exit 130")
{
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Acquire;
    options.host = "mvlc-test";
    options.profilePath = "expected.mwwscp";
    options.channel = 29U;
    FakeTunerControl control(
        fidget::GuidedTunerOwnershipState::Idle, false, true);
    std::istringstream input("yes\ny\n");
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliAcquire(
        options,
        control,
        input,
        output,
        errors,
        [&control] { return control.startAcquisitionCommands > 0; },
        [] { return fidget::CliSessionWaitResult::Interrupted; });

    CHECK(exitCode == 130);
    CHECK(control.startAcquisitionCommands == 1);
    CHECK(control.stopAcquisitionCommands == 1);
    CHECK(control.releaseCommands == 1);
    CHECK(output.str().find("session: released\n") != std::string::npos);
}

TEST_CASE("recover reports a missing journal as a successful no-op")
{
    TemporaryCrateProject project;
    FakeTunerControl control;
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Recover;
    options.projectPath = project.path;
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliRecover(
        options, control, input, output, errors, [] { return false; });

    CHECK(exitCode == 0);
    CHECK(output.str().find("no recovery journal for this project")
          != std::string::npos);
    CHECK(control.recoveryStatusCommands == 0);
    CHECK(control.recoveryCommands == 0);
}

TEST_CASE("recover refuses malformed evidence without deleting it")
{
    TemporaryCrateProject project;
    FakeTunerControl control;
    control.recoveryJournalStatus = fidget::RecoveryJournalStatus::Malformed;
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Recover;
    options.projectPath = project.path;
    std::istringstream input("y\n");
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliRecover(
        options, control, input, output, errors, [] { return false; });

    CHECK(exitCode == 1);
    CHECK(errors.str().find("malformed and was retained")
          != std::string::npos);
    CHECK(control.recoveryStatusCommands == 0);
    CHECK(control.recoveryCommands == 0);
}

TEST_CASE("recover defaults to No on closed input with no recovery write")
{
    TemporaryCrateProject project;
    FakeTunerControl control;
    control.recoveryJournalStatus = fidget::RecoveryJournalStatus::Pending;
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Recover;
    options.projectPath = project.path;
    std::istringstream input;
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliRecover(
        options, control, input, output, errors, [] { return false; });

    CHECK(exitCode == 1);
    CHECK(control.recoveryStatusCommands == 1);
    CHECK(control.recoveryCommands == 0);
    CHECK(output.str().find("recovery_preview_restore: required")
          != std::string::npos);
    CHECK(output.str().find("recovery_source_restore: required")
          != std::string::npos);
    CHECK(output.str().find("Recover journaled tuner state [y/N]")
          != std::string::npos);
    CHECK(output.str().find("recovery: not run") != std::string::npos);
}

TEST_CASE("recover prints the decoded plan and successful steps")
{
    TemporaryCrateProject project;
    FakeTunerControl control;
    control.recoveryJournalStatus = fidget::RecoveryJournalStatus::Pending;
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Recover;
    options.projectPath = project.path;
    std::istringstream input("y\n");
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliRecover(
        options, control, input, output, errors, [] { return false; });

    CHECK(exitCode == 0);
    CHECK(control.recoveryStatusCommands == 1);
    CHECK(control.recoveryCommands == 1);
    CHECK(output.str().find("recovery_endpoint: mvlc-test:32768")
          != std::string::npos);
    CHECK(output.str().find("recovery_step: fingerprint passed")
          != std::string::npos);
    CHECK(output.str().find("Recovered the tuner-owned diagnostic orphan")
          != std::string::npos);
}

TEST_CASE("recover reports a foreign fingerprint as failure")
{
    TemporaryCrateProject project;
    FakeTunerControl control;
    control.recoveryJournalStatus = fidget::RecoveryJournalStatus::Pending;
    control.recoveryOutcome =
        fidget::DiagnosticOrphanRecoveryState::ForeignOrMismatched;
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Recover;
    options.projectPath = project.path;
    std::istringstream input("y\n");
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliRecover(
        options, control, input, output, errors, [] { return false; });

    CHECK(exitCode == 1);
    CHECK(control.recoveryCommands == 1);
    CHECK(output.str().find("recovery_step: fingerprint failed")
          != std::string::npos);
    CHECK(control.CurrentSnapshot()->recoveryRecordAvailable);
}

TEST_CASE("confirmation prompts discard a pending blank line before y")
{
    TemporaryCrateProject project;
    FakeTunerControl control;
    control.recoveryJournalStatus = fidget::RecoveryJournalStatus::Pending;
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Recover;
    options.projectPath = project.path;
    std::istringstream input("\ny\n");
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliRecover(
        options, control, input, output, errors, [] { return false; });

    CHECK(exitCode == 0);
    CHECK(control.recoveryCommands == 1);
}

TEST_CASE("acquire reports preview and source restoration from final cleanup")
{
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Acquire;
    options.host = "mvlc-test";
    options.profilePath = "expected.mwwscp";
    options.channel = 29U;
    FakeTunerControl control(
        fidget::GuidedTunerOwnershipState::Idle, false, true);
    std::istringstream input(
        "yes\ny\ns3\np 0x611a 250\n\n");
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliAcquire(
        options,
        control,
        input,
        output,
        errors,
        [] { return false; },
        [] { return fidget::CliSessionWaitResult::InputReady; });

    CHECK(exitCode == 0);
    CHECK(output.str().find(
              "cleanup_preview: attempted=yes verified=yes")
          != std::string::npos);
    CHECK(output.str().find(
              "cleanup_source: attempted=yes verified=yes")
          != std::string::npos);
}

TEST_CASE("silent watched channels emit one liveness warning")
{
    fidget::CliOptions options;
    options.command = fidget::CliCommand::Acquire;
    options.host = "mvlc-test";
    options.profilePath = "expected.mwwscp";
    options.channel = 29U;
    options.seconds = 16U;
    FakeTunerControl control(
        fidget::GuidedTunerOwnershipState::Idle, false, true);
    control.acquisitionSilent = true;
    std::istringstream input("yes\ny\n");
    std::ostringstream output;
    std::ostringstream errors;

    const int exitCode = fidget::RunCliAcquire(
        options,
        control,
        input,
        output,
        errors,
        [] { return false; },
        [] { return fidget::CliSessionWaitResult::OneSecondElapsed; });

    CHECK(exitCode == 0);
    CHECK(CountOccurrences(output.str(), "liveness_warning:") == 1U);
    CHECK(output.str().find("not a safety gate") != std::string::npos);
}
