#include "ui/AcquisitionStage.h"

#include "core/Acquisition.h"

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
