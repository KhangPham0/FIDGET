#include "ui/SessionStage.h"

#include "imgui.h"

#include <cstdint>

namespace fidget {
namespace {

const char* OwnershipName(GuidedTunerOwnershipState ownership)
{
    switch (ownership)
    {
    case GuidedTunerOwnershipState::Disconnected:
        return "Disconnected";
    case GuidedTunerOwnershipState::Checking:
        return "Checking";
    case GuidedTunerOwnershipState::Idle:
        return "Idle";
    case GuidedTunerOwnershipState::InUse:
        return "In use";
    case GuidedTunerOwnershipState::SessionOpen:
        return "Session open";
    case GuidedTunerOwnershipState::OwnershipLost:
        return "Ownership lost";
    case GuidedTunerOwnershipState::RecoveryRequired:
        return "Recovery required";
    case GuidedTunerOwnershipState::Failed:
        return "Check failed";
    }
    return "Unknown";
}

ImVec4 OwnershipColor(
    GuidedTunerOwnershipState ownership,
    const Theme& theme)
{
    switch (ownership)
    {
    case GuidedTunerOwnershipState::Idle:
    case GuidedTunerOwnershipState::SessionOpen:
        return theme.statusGood;
    case GuidedTunerOwnershipState::Checking:
        return theme.accent;
    case GuidedTunerOwnershipState::Disconnected:
        return theme.textDisabled;
    case GuidedTunerOwnershipState::InUse:
        return theme.statusWarning;
    case GuidedTunerOwnershipState::OwnershipLost:
    case GuidedTunerOwnershipState::RecoveryRequired:
    case GuidedTunerOwnershipState::Failed:
        return theme.statusError;
    }
    return theme.textDisabled;
}

} // namespace

void SessionStage::Draw(
    ITunerControl& tunerControl,
    const TunerSnapshot& snapshot,
    const Theme& theme)
{
    ImGui::TextUnformatted("Ownership session");
    ImGui::BeginChild(
        "session_status",
        ImVec2(0.0F, 146.0F),
        ImGuiChildFlags_Borders);
    ImGui::TextColored(
        OwnershipColor(snapshot.ownership, theme),
        "%s",
        OwnershipName(snapshot.ownership));
    ImGui::Separator();
    ImGui::Text(
        "Command endpoint: %s:%u",
        snapshot.mvlcHost.empty() ? "not configured" : snapshot.mvlcHost.c_str(),
        static_cast<unsigned>(snapshot.mvlcCommandPort));

    if (snapshot.controllerReadingsValid)
    {
        ImGui::Text(
            "MVLC hardware ID: 0x%08X",
            static_cast<unsigned>(snapshot.mvlcHardwareId));
    }
    else
    {
        ImGui::TextDisabled("MVLC hardware ID: not read");
    }

    if (snapshot.ownership != GuidedTunerOwnershipState::Disconnected)
    {
        ImGui::Text(
            "MVLC firmware: 0x%08X",
            static_cast<unsigned>(snapshot.mvlcFirmwareRevision));
        ImGui::Text(
            "DAQ mode: 0x%08X",
            static_cast<unsigned>(snapshot.mvlcDaqMode));
    }
    else
    {
        ImGui::TextDisabled("MVLC firmware and DAQ mode: not read");
    }
    ImGui::EndChild();

    const bool checking =
        snapshot.ownership == GuidedTunerOwnershipState::Checking;
    const bool sessionOpen =
        snapshot.ownership == GuidedTunerOwnershipState::SessionOpen;
    const bool mayCheck = snapshot.projectActive && !checking && !sessionOpen;

    ImGui::BeginDisabled(!mayCheck);
    if (ImGui::Button("Check MVLC status"))
    {
        tunerControl.Submit(CheckStatusCommand{});
    }
    ImGui::EndDisabled();

    ImGui::Spacing();
    ImGui::TextWrapped(
        "An idle DAQ-mode register cannot prove that an idle MVME process "
        "has released USB. Confirmation is still required before opening "
        "a session.");

    bool handoffConfirmed = snapshot.mvmeHandoffConfirmed;
    const bool mayConfirm =
        snapshot.ownership == GuidedTunerOwnershipState::Idle;
    ImGui::BeginDisabled(!mayConfirm);
    if (ImGui::Checkbox(
            "I stopped the MVME run and completely quit MVME",
            &handoffConfirmed))
    {
        tunerControl.Submit(
            SetMvmeHandoffConfirmedCommand{handoffConfirmed});
    }
    ImGui::EndDisabled();

    const bool mayOpen = mayConfirm && snapshot.mvmeHandoffConfirmed;
    ImGui::BeginDisabled(!mayOpen);
    if (ImGui::Button("Open session"))
    {
        tunerControl.Submit(OpenSessionCommand{});
    }
    ImGui::EndDisabled();

    if (sessionOpen
        || snapshot.ownership == GuidedTunerOwnershipState::OwnershipLost)
    {
        ImGui::SameLine();
        if (ImGui::Button("Release session"))
        {
            tunerControl.Submit(ReleaseSessionCommand{});
        }
    }

    if (!snapshot.statusMessages.empty())
    {
        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextWrapped(
            "%s", snapshot.statusMessages.back().summary.c_str());
        if (!snapshot.statusMessages.back().detail.empty())
        {
            ImGui::TextDisabled(
                "%s", snapshot.statusMessages.back().detail.c_str());
        }
    }
}

} // namespace fidget
