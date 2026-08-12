#include "ui/AcquisitionStage.h"

#include "core/Acquisition.h"
#include "core/ScpRegistry.h"

#include "imgui.h"
#include "implot.h"

#include <algorithm>
#include <cstddef>
#include <cstdio>
#include <string>

namespace fidget {
namespace {

constexpr std::size_t VisibleTrailCount = 12U;

bool IsAcquisitionActive(const DiagnosticAcquisitionState state) noexcept
{
    return state == DiagnosticAcquisitionState::Starting
        || state == DiagnosticAcquisitionState::Running
        || state == DiagnosticAcquisitionState::Stopping;
}

const char* AcquisitionStateName(
    const DiagnosticAcquisitionState state) noexcept
{
    switch (state)
    {
    case DiagnosticAcquisitionState::NotRun:
        return "not run";
    case DiagnosticAcquisitionState::Starting:
        return "starting";
    case DiagnosticAcquisitionState::Running:
        return "running";
    case DiagnosticAcquisitionState::Stopping:
        return "stopping";
    case DiagnosticAcquisitionState::Stopped:
        return "stopped with verified cleanup";
    case DiagnosticAcquisitionState::Failed:
        return "failed";
    }
    return "unknown";
}

const char* SourceName(const std::uint8_t source) noexcept
{
    switch (source)
    {
    case 0U:
        return "ADC";
    case 1U:
        return "Deconvoluted";
    case 2U:
        return "Timing filter";
    case 3U:
        return "Main shaper";
    default:
        return "Unknown";
    }
}

const char* SourceChangeStateName(
    const DiagnosticSourceChangeState state) noexcept
{
    switch (state)
    {
    case DiagnosticSourceChangeState::NotRun:
        return "not run";
    case DiagnosticSourceChangeState::Applying:
        return "applying";
    case DiagnosticSourceChangeState::Passed:
        return "passed";
    case DiagnosticSourceChangeState::Failed:
        return "failed";
    }
    return "unknown";
}

const char* PreviewStateName(
    const DiagnosticParameterPreviewState state) noexcept
{
    switch (state)
    {
    case DiagnosticParameterPreviewState::NotRun:
        return "not run";
    case DiagnosticParameterPreviewState::Applying:
        return "applying";
    case DiagnosticParameterPreviewState::PreviewActive:
        return "preview active";
    case DiagnosticParameterPreviewState::Restoring:
        return "restoring";
    case DiagnosticParameterPreviewState::Restored:
        return "restored";
    case DiagnosticParameterPreviewState::Failed:
        return "failed";
    }
    return "unknown";
}

ImVec4 SourceChangeColor(
    const DiagnosticSourceChangeState state,
    const Theme& theme) noexcept
{
    if (state == DiagnosticSourceChangeState::Passed)
    {
        return theme.statusGood;
    }
    if (state == DiagnosticSourceChangeState::Failed)
    {
        return theme.statusError;
    }
    return theme.accent;
}

ImVec4 PreviewColor(
    const DiagnosticParameterPreviewState state,
    const Theme& theme) noexcept
{
    if (state == DiagnosticParameterPreviewState::PreviewActive
        || state == DiagnosticParameterPreviewState::Restored)
    {
        return theme.statusGood;
    }
    if (state == DiagnosticParameterPreviewState::Failed)
    {
        return theme.statusError;
    }
    return theme.accent;
}

ImVec4 AcquisitionStateColor(
    const DiagnosticAcquisitionResult& acquisition,
    const Theme& theme) noexcept
{
    if (acquisition.foreignControllerDetected)
    {
        return theme.statusError;
    }
    if (acquisition.communicationUncertain)
    {
        return theme.statusWarning;
    }
    switch (acquisition.state)
    {
    case DiagnosticAcquisitionState::Running:
    case DiagnosticAcquisitionState::Stopped:
        return theme.statusGood;
    case DiagnosticAcquisitionState::Starting:
    case DiagnosticAcquisitionState::Stopping:
        return theme.accent;
    case DiagnosticAcquisitionState::Failed:
        return theme.statusError;
    case DiagnosticAcquisitionState::NotRun:
        return theme.textDisabled;
    }
    return theme.textDisabled;
}

const MdppChannelHistorySnapshot* RequestedHistory(
    const DiagnosticStreamSnapshot& stream)
{
    if (!stream.requestedTarget.moduleObserved)
    {
        return nullptr;
    }
    const auto found = stream.histories.find({
        static_cast<std::uint8_t>(stream.requestedTarget.moduleId),
        stream.requestedTarget.requestedChannel,
    });
    return found == stream.histories.end() ? nullptr : &found->second;
}

ImVec4 WithAlpha(ImVec4 color, const float alpha) noexcept
{
    color.w = alpha;
    return color;
}

void DrawWaveformPlot(
    const MdppChannelHistorySnapshot* history,
    const std::vector<std::int16_t>& referenceSamples,
    const Theme& theme)
{
    if (!ImPlot::BeginPlot("Requested channel waveform", ImVec2(-1.0f, 360.0f)))
    {
        return;
    }

    ImPlot::SetupAxes(
        "sample index",
        "ADC value",
        ImPlotAxisFlags_AutoFit,
        ImPlotAxisFlags_AutoFit);

    if (history != nullptr && !history->waveforms.empty())
    {
        const auto& waveforms = history->waveforms;
        const std::size_t trailCount = std::min(
            VisibleTrailCount,
            waveforms.size() > 0U ? waveforms.size() - 1U : 0U);
        const std::size_t firstTrail = waveforms.size() - trailCount - 1U;
        for (std::size_t index = firstTrail;
             index + 1U < waveforms.size();
             ++index)
        {
            const auto& waveform = waveforms[index];
            if (waveform.samples.empty())
            {
                continue;
            }
            const std::size_t age = waveforms.size() - 1U - index;
            const float ageFraction = trailCount == 0U
                ? 1.0f
                : static_cast<float>(trailCount - age + 1U)
                    / static_cast<float>(trailCount);
            const float alpha = 0.08f + 0.34f * ageFraction;
            char label[48];
            std::snprintf(
                label,
                sizeof(label),
                "##trail_%llu",
                static_cast<unsigned long long>(waveform.sequence));
            ImPlot::PlotLine(
                label,
                waveform.samples.data(),
                static_cast<int>(waveform.samples.size()),
                1.0,
                0.0,
                {ImPlotProp_LineColor, WithAlpha(theme.waveformTrail, alpha),
                 ImPlotProp_LineWeight, 1.0f});
        }

        const auto& latest = waveforms.back();
        if (!latest.samples.empty())
        {
            ImPlot::PlotLine(
                "Live",
                latest.samples.data(),
                static_cast<int>(latest.samples.size()),
                1.0,
                0.0,
                {ImPlotProp_LineColor, theme.waveformLive,
                 ImPlotProp_LineWeight, 2.0f});
        }
    }

    if (!referenceSamples.empty())
    {
        ImPlot::PlotLine(
            "Frozen reference",
            referenceSamples.data(),
            static_cast<int>(referenceSamples.size()),
            1.0,
            0.0,
            {ImPlotProp_LineColor, theme.referenceTrace,
             ImPlotProp_LineWeight, 2.0f});
    }

    ImPlot::EndPlot();
}

} // namespace

void AcquisitionStage::Draw(
    ITunerControl& tunerControl,
    const TunerSnapshot& snapshot,
    const Theme& theme)
{
    const auto& acquisition = snapshot.diagnosticAcquisition;
    const auto& stream = snapshot.diagnosticStream;
    const bool acquisitionActive = IsAcquisitionActive(acquisition.state);
    if (acquisitionActive)
    {
        m_selectedChannel = static_cast<int>(acquisition.requestedChannel);
    }

    ImGui::TextUnformatted("Direct MDPP-32 SCP diagnostic acquisition");
    ImGui::TextWrapped(
        "FIDGET installs a journaled MVLC readout stack, isolates every "
        "other configured module, and watches the full ownership fingerprint "
        "while the data receiver drains independently.");
    ImGui::Spacing();

    ImGui::BeginDisabled(acquisitionActive);
    const int previousChannel = m_selectedChannel;
    ImGui::SliderInt("Physical channel", &m_selectedChannel, 0, 31);
    ImGui::EndDisabled();
    if (m_selectedChannel != previousChannel)
    {
        m_referenceSamples.clear();
        m_referenceChannel = -1;
    }
    if (acquisitionActive)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("locked while acquisition is active");
    }

    std::string startDisabledReason;
    if (acquisitionActive)
    {
        startDisabledReason = "Acquisition is already active.";
    }
    else if (snapshot.activeOperation != GuidedTunerOperation::None)
    {
        startDisabledReason = "Another controlled operation is running.";
    }
    else if (snapshot.ownership != GuidedTunerOwnershipState::SessionOpen)
    {
        startDisabledReason = "Open the ownership session first.";
    }
    else if (!snapshot.deterministicStartupPassed)
    {
        startDisabledReason =
            "Complete deterministic startup for this target first.";
    }

    ImGui::BeginDisabled(!startDisabledReason.empty());
    if (ImGui::Button("Start journaled acquisition"))
    {
        m_referenceSamples.clear();
        m_referenceChannel = -1;
        tunerControl.Submit(StartDiagnosticAcquisitionCommand{
            static_cast<std::uint16_t>(m_selectedChannel),
        });
    }
    ImGui::EndDisabled();
    if (acquisitionActive)
    {
        ImGui::SameLine();
        ImGui::BeginDisabled(
            acquisition.state == DiagnosticAcquisitionState::Stopping);
        ImGui::PushStyleColor(ImGuiCol_Button, theme.statusWarning);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.highlight);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.statusWarning);
        if (ImGui::Button("Stop with verified cleanup"))
        {
            tunerControl.Submit(StopDiagnosticAcquisitionCommand{});
        }
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
    }
    if (!startDisabledReason.empty() && !acquisitionActive)
    {
        ImGui::TextDisabled(
            "Acquisition disabled: %s", startDisabledReason.c_str());
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Live status");
    ImGui::TextColored(
        AcquisitionStateColor(acquisition, theme),
        "%s",
        AcquisitionStateName(acquisition.state));
    if (!acquisition.message.empty())
    {
        ImGui::SameLine();
        ImGui::TextWrapped("%s", acquisition.message.c_str());
    }
    ImGui::Text(
        "Datagrams: %llu total | %.1f/s",
        static_cast<unsigned long long>(stream.datagramsReceived),
        stream.datagramsPerSecond);
    ImGui::Text(
        "Waveforms: %llu total | %.1f/s | Packet loss: %llu | Decode errors: %llu",
        static_cast<unsigned long long>(
            stream.decoderStats.decodedWaveforms),
        stream.waveformsPerSecond,
        static_cast<unsigned long long>(
            stream.decoderStats.lostEthernetPackets),
        static_cast<unsigned long long>(stream.decoderStats.malformedWords));
    if (stream.channelWaveformTotals.empty())
    {
        ImGui::TextDisabled("Channel hit totals: no waveform hits yet");
    }
    else
    {
        ImGui::TextUnformatted("Channel hit totals:");
        ImGui::SameLine();
        for (std::size_t index = 0U;
             index < stream.channelWaveformTotals.size();
             ++index)
        {
            if (index != 0U)
            {
                ImGui::SameLine(0.0f, 12.0f);
            }
            const auto& channel = stream.channelWaveformTotals[index];
            ImGui::Text(
                "%u=%llu",
                static_cast<unsigned>(channel.channel),
                static_cast<unsigned long long>(channel.total));
        }
    }
    if (acquisition.communicationUncertain)
    {
        ImGui::TextColored(
            theme.statusWarning,
            "Communication uncertain. Writes are frozen; waveform draining continues.");
    }
    if (acquisition.foreignControllerDetected)
    {
        ImGui::TextColored(
            theme.statusError,
            "Ownership fingerprint changed. FIDGET detached without cleanup writes.");
    }
    if (!stream.receiverError.empty())
    {
        ImGui::TextColored(
            theme.statusError,
            "Data receiver: %s",
            stream.receiverError.c_str());
    }

    const auto* history = RequestedHistory(stream);
    if (history == nullptr || history->waveforms.empty())
    {
        ImGui::TextDisabled(
            "Waiting for the first waveform on physical channel %d.",
            m_selectedChannel);
    }
    else
    {
        ImGui::Text(
            "Requested channel captures: %llu | Showing %zu of %zu retained waveform(s)",
            static_cast<unsigned long long>(history->totalCaptured),
            std::min(VisibleTrailCount + 1U, history->waveforms.size()),
            history->waveforms.size());
    }

    ImGui::BeginDisabled(history == nullptr || history->waveforms.empty());
    if (ImGui::Button("Freeze latest as reference") && history != nullptr
        && !history->waveforms.empty())
    {
        m_referenceSamples = history->waveforms.back().samples;
        m_referenceChannel = m_selectedChannel;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(m_referenceSamples.empty());
    if (ImGui::Button("Clear frozen reference"))
    {
        m_referenceSamples.clear();
        m_referenceChannel = -1;
    }
    ImGui::EndDisabled();
    if (!m_referenceSamples.empty())
    {
        ImGui::SameLine();
        ImGui::TextColored(
            theme.referenceTrace,
            "reference: channel %d, %zu samples",
            m_referenceChannel,
            m_referenceSamples.size());
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Tune loop");
    const bool acquisitionRunning =
        acquisition.state == DiagnosticAcquisitionState::Running;
    const auto& sourceChange = snapshot.diagnosticSourceChange;
    const auto& preview = snapshot.diagnosticParameterPreview;
    const bool sourceBusy =
        sourceChange.state == DiagnosticSourceChangeState::Applying;
    const bool previewBusy =
        preview.state == DiagnosticParameterPreviewState::Applying
        || preview.state == DiagnosticParameterPreviewState::Restoring;
    const auto selectedQuad = static_cast<unsigned>(m_selectedChannel / 4);
    ImGui::Text(
        "Waveform source for quad %u (channels %u-%u)",
        selectedQuad,
        selectedQuad * 4U,
        selectedQuad * 4U + 3U);

    ImGui::BeginDisabled(
        !acquisitionRunning || sourceBusy || previewBusy
        || preview.previewActive || acquisition.communicationUncertain);
    for (std::uint8_t source = 0U; source < 4U; ++source)
    {
        if (source != 0U)
        {
            ImGui::SameLine();
        }
        if (ImGui::Button(SourceName(source)))
        {
            tunerControl.Submit(ChangeDiagnosticSourceCommand{source});
        }
    }
    ImGui::EndDisabled();
    if (preview.previewActive)
    {
        ImGui::TextDisabled(
            "Source switching is locked until the active preview is restored.");
    }
    if (sourceChange.state != DiagnosticSourceChangeState::NotRun)
    {
        ImGui::TextColored(
            SourceChangeColor(sourceChange.state, theme),
            "%s",
            SourceChangeStateName(sourceChange.state));
        ImGui::SameLine();
        ImGui::TextWrapped("%s", sourceChange.message.c_str());
        if (sourceChange.originalCaptured)
        {
            ImGui::Text(
                "Quad %u | configuration 0x%04X -> 0x%04X | readback 0x%04X",
                static_cast<unsigned>(sourceChange.selectedQuad),
                static_cast<unsigned>(sourceChange.originalConfiguration),
                static_cast<unsigned>(sourceChange.requestedConfiguration),
                static_cast<unsigned>(sourceChange.appliedReadback));
            ImGui::Text(
                "Write %s | selector parked %s | MDPP resumed %s | DAQ resumed %s (0x%08X)",
                sourceChange.writeVerified ? "verified" : "not verified",
                sourceChange.selectorParkedAtQuadZero ? "yes" : "NO",
                sourceChange.acquisitionResumed ? "yes" : "NO",
                sourceChange.daqModeResumed ? "yes" : "NO",
                static_cast<unsigned>(sourceChange.daqModeReadback));
        }
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Temporary SCP parameter preview");
    ImGui::TextWrapped(
        "The original value is journaled before the temporary write, then "
        "restored explicitly or during verified Stop cleanup. Applying a "
        "preview freezes the latest trace as the gold reference.");

    m_parameterDefinitionIndex = std::clamp(
        m_parameterDefinitionIndex,
        0,
        static_cast<int>(Fw2051ScpSettingRegistry.size()) - 1);
    const auto& definition = Fw2051ScpSettingRegistry[
        static_cast<std::size_t>(m_parameterDefinitionIndex)];
    ImGui::BeginDisabled(preview.previewActive || previewBusy);
    ImGui::SetNextItemWidth(280.0F);
    if (ImGui::BeginCombo("Preview parameter", definition.name))
    {
        for (std::size_t index = 0U;
             index < Fw2051ScpSettingRegistry.size();
             ++index)
        {
            const auto& candidate = Fw2051ScpSettingRegistry[index];
            char label[128]{};
            std::snprintf(
                label,
                sizeof(label),
                "%s (0x%04X)",
                candidate.name,
                static_cast<unsigned>(candidate.registerOffset));
            const bool selected =
                m_parameterDefinitionIndex == static_cast<int>(index);
            if (ImGui::Selectable(label, selected))
            {
                m_parameterDefinitionIndex = static_cast<int>(index);
                m_proposedParameterValue = candidate.minimumValue;
                m_parameterValueEdited = false;
            }
            if (selected)
            {
                ImGui::SetItemDefaultFocus();
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SetNextItemWidth(360.0F);
    if (ImGui::SliderInt(
            "Validated range",
            &m_proposedParameterValue,
            static_cast<int>(definition.minimumValue),
            static_cast<int>(definition.maximumValue)))
    {
        m_parameterValueEdited = true;
    }
    ImGui::SetNextItemWidth(140.0F);
    if (ImGui::InputInt(
            "Register value", &m_proposedParameterValue, 1, 10))
    {
        m_parameterValueEdited = true;
    }
    ImGui::EndDisabled();

    std::string previewValidation;
    if (m_proposedParameterValue < 0
        || m_proposedParameterValue > 0xFFFF)
    {
        previewValidation = "The register value must fit in 16 bits.";
    }
    else
    {
        previewValidation = ValidateFw2051ScpSettingValue(
            definition,
            static_cast<std::uint16_t>(m_proposedParameterValue),
            "preview ");
    }
    ImGui::Text(
        "Register 0x%04X | allowed %u-%u | quad %u",
        static_cast<unsigned>(definition.registerOffset),
        static_cast<unsigned>(definition.minimumValue),
        static_cast<unsigned>(definition.maximumValue),
        selectedQuad);
    if (definition.valueRule == Fw2051ScpValueRule::EvenRange)
    {
        ImGui::SameLine();
        ImGui::TextDisabled("| even values only");
    }
    if (definition.dependencyRule != Fw2051ScpDependencyRule::None)
    {
        ImGui::Text(
            "Live dependency: value must be %s %s at 0x%04X",
            Fw2051ScpDependencyRelation(definition.dependencyRule),
            definition.dependencyName,
            static_cast<unsigned>(definition.dependencyRegister));
    }
    if (!previewValidation.empty())
    {
        ImGui::TextColored(
            theme.statusError, "%s", previewValidation.c_str());
    }

    if (!preview.previewActive)
    {
        ImGui::BeginDisabled(
            !acquisitionRunning || sourceBusy || previewBusy
            || !m_parameterValueEdited || !previewValidation.empty()
            || acquisition.communicationUncertain);
        ImGui::PushStyleColor(ImGuiCol_Button, theme.accent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.highlight);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.accent);
        if (ImGui::Button("Freeze reference and apply temporary preview"))
        {
            if (history != nullptr && !history->waveforms.empty())
            {
                m_referenceSamples = history->waveforms.back().samples;
                m_referenceChannel = m_selectedChannel;
            }
            tunerControl.Submit(ApplyDiagnosticPreviewCommand{
                definition.registerOffset,
                static_cast<std::uint16_t>(m_proposedParameterValue),
            });
        }
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
        if (!m_parameterValueEdited)
        {
            ImGui::SameLine();
            ImGui::TextDisabled("Choose a proposed value first.");
        }
    }
    else
    {
        ImGui::BeginDisabled(
            !acquisitionRunning || sourceBusy || previewBusy
            || acquisition.communicationUncertain);
        if (ImGui::Button("Restore original parameter"))
        {
            tunerControl.Submit(RestoreDiagnosticPreviewCommand{});
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextColored(
            theme.statusWarning,
            "Temporary preview is active and will auto-restore on Stop.");
    }

    if (preview.state != DiagnosticParameterPreviewState::NotRun)
    {
        ImGui::TextColored(
            PreviewColor(preview.state, theme),
            "%s",
            PreviewStateName(preview.state));
        ImGui::SameLine();
        ImGui::TextWrapped("%s", preview.message.c_str());
        if (preview.originalCaptured)
        {
            ImGui::Text(
                "%s 0x%04X | quad %u | %u -> %u | apply readback %u",
                preview.settingName.c_str(),
                static_cast<unsigned>(preview.registerOffset),
                static_cast<unsigned>(preview.selectedQuad),
                static_cast<unsigned>(preview.originalValue),
                static_cast<unsigned>(preview.requestedValue),
                static_cast<unsigned>(preview.appliedReadback));
            if (preview.dependencyChecked)
            {
                ImGui::Text(
                    "Dependency verified: %s 0x%04X = %u",
                    preview.dependencyName.c_str(),
                    static_cast<unsigned>(preview.dependencyRegister),
                    static_cast<unsigned>(preview.dependencyValue));
            }
            ImGui::Text(
                "Apply %.3f ms | restore %.3f ms | selector parked %s | acquisition resumed %s",
                preview.applyDurationMicroseconds / 1000.0,
                preview.restoreDurationMicroseconds / 1000.0,
                preview.selectorParkedAtQuadZero ? "yes" : "NO",
                preview.acquisitionResumed
                    ? "yes"
                    : preview.automaticallyRestoredOnStop
                        ? "not requested"
                        : "NO");
            if (preview.restoreVerified)
            {
                ImGui::Text(
                    "Restored readback %u%s",
                    static_cast<unsigned>(preview.restoredReadback),
                    preview.automaticallyRestoredOnStop
                        ? " (automatic Stop restore)"
                        : "");
            }
        }
    }

    DrawWaveformPlot(history, m_referenceSamples, theme);

    if (acquisition.state == DiagnosticAcquisitionState::Stopped)
    {
        ImGui::TextColored(
            snapshot.cleanupVerified ? theme.statusGood : theme.statusError,
            "Cleanup: selected module %s | isolation %zu/%zu | MVLC stack %s | journal %s",
            acquisition.moduleStopSent ? "stopped" : "not verified",
            acquisition.nonTargetModulesVerifiedStoppedOnCleanup,
            acquisition.nonTargetModulesQuiesced,
            acquisition.readoutStackDisabled ? "zeroed" : "not verified",
            acquisition.recoveryJournalRemoved ? "removed" : "retained");
    }
}

} // namespace fidget
