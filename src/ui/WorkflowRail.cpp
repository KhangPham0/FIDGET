#include "ui/WorkflowRail.h"

#include "imgui.h"

#include <array>
#include <cstddef>

namespace fidget {
namespace {

constexpr std::array<const char*, 7> StepNames{
    "Project",
    "Ownership",
    "Profile",
    "Startup audit",
    "Configuration",
    "Startup plan",
    "Acquisition",
};

ImVec4 ToneColor(GuidedTunerTone tone, const Theme& theme)
{
    switch (tone)
    {
    case GuidedTunerTone::Neutral:
        return theme.textDisabled;
    case GuidedTunerTone::Working:
        return theme.accent;
    case GuidedTunerTone::Ready:
        return theme.statusGood;
    case GuidedTunerTone::Attention:
        return theme.statusWarning;
    case GuidedTunerTone::Blocked:
        return theme.statusError;
    }
    return theme.textDisabled;
}

} // namespace

bool DrawWorkflowRail(
    const GuidedTunerDecision& decision,
    const Theme& theme)
{
    for (std::size_t index = 0U; index < StepNames.size(); ++index)
    {
        const std::size_t step = index + 1U;
        ImVec4 color = theme.textDisabled;
        const char* marker = "○";
        if (step < decision.step)
        {
            color = theme.statusGood;
            marker = "●";
        }
        else if (step == decision.step)
        {
            color = ToneColor(decision.tone, theme);
            marker = "●";
        }

        ImGui::TextColored(
            color, "%s  %zu. %s", marker, step, StepNames[index]);
        if (step == decision.step)
        {
            ImGui::Indent(19.0F);
            ImGui::TextWrapped("%s", GuidedTunerStageName(decision.stage));
            ImGui::Unindent(19.0F);
        }
    }

    if (decision.nextAction == GuidedTunerAction::None)
    {
        return false;
    }

    ImGui::Spacing();
    ImGui::PushStyleColor(ImGuiCol_Button, theme.accent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.accentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.accentActive);
    ImGui::PushStyleColor(ImGuiCol_Text, theme.textOnAccent);
    const bool pressed = ImGui::Button(
        GuidedTunerActionName(decision.nextAction),
        ImVec2(-1.0F, 0.0F));
    ImGui::PopStyleColor(4);
    return pressed;
}

} // namespace fidget
