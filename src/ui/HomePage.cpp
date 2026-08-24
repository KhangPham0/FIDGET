#include "ui/HomePage.h"

#include "core/TunerTarget.h"
#include "ui/fonts/IconsFontAwesome5.h"

#include "imgui.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <filesystem>
#include <string>

namespace fidget {
namespace {

constexpr UiDialogFilter MvmeWorkspaceFilter{
    "MVME workspace",
    "vme",
};

constexpr float PageMaximumWidth = 1198.0F;
constexpr float CardHeight = 230.0F;
constexpr float CardGap = 18.0F;

bool IsHomePage(const GuiPage page) noexcept
{
    return page == GuiPage::HomeDisconnected
        || page == GuiPage::HomeReady
        || page == GuiPage::HomeSshExpanded;
}

ImVec4 StatusColor(const TunerStatusLevel level, const Theme& theme)
{
    switch (level)
    {
    case TunerStatusLevel::Information:
        return theme.textDisabled;
    case TunerStatusLevel::Success:
        return theme.statusGood;
    case TunerStatusLevel::Warning:
        return theme.statusWarning;
    case TunerStatusLevel::Error:
        return theme.statusError;
    }
    return theme.textDisabled;
}

ImVec4 ProbeColor(const TargetProbeOutcome outcome, const Theme& theme)
{
    switch (outcome)
    {
    case TargetProbeOutcome::VerifiedIdle:
        return theme.statusGood;
    case TargetProbeOutcome::ControllerDaqActive:
    case TargetProbeOutcome::TargetAcquisitionActive:
        return theme.statusWarning;
    case TargetProbeOutcome::NotRun:
    case TargetProbeOutcome::InProgress:
        return theme.textDisabled;
    default:
        return theme.statusError;
    }
}

void PushCardStyle(const Theme& theme)
{
    ImGui::PushStyleColor(ImGuiCol_ChildBg, theme.panelBackground);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.border);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0F, 18.0F));
}

void PopCardStyle()
{
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

void BeginCard(const char* identifier, const ImVec2 size, const Theme& theme)
{
    PushCardStyle(theme);
    ImGui::BeginChild(identifier, size, true);
    PopCardStyle();
}

void DrawCardTitle(const char* title, const Fonts& fonts)
{
    if (fonts.ui != nullptr)
        ImGui::PushFont(fonts.ui, 18.0F);
    ImGui::TextUnformatted(title);
    if (fonts.ui != nullptr)
        ImGui::PopFont();
    ImGui::Spacing();
}

void DrawValidationMessage(
    const std::string& message,
    const bool valid,
    const Theme& theme)
{
    if (message.empty())
        return;
    ImGui::PushStyleColor(
        ImGuiCol_Text, valid ? theme.textDisabled : theme.statusError);
    ImGui::TextWrapped("%s", message.c_str());
    ImGui::PopStyleColor();
}

bool DrawPrimaryButton(
    const char* label,
    const bool enabled,
    const Theme& theme,
    const ImVec2 size = ImVec2(0.0F, 0.0F))
{
    ImGui::PushStyleColor(ImGuiCol_Button, theme.accentActive);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.accentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.accent);
    ImGui::BeginDisabled(!enabled);
    const bool pressed = ImGui::Button(label, size);
    ImGui::EndDisabled();
    ImGui::PopStyleColor(3);
    return pressed && enabled;
}

bool CurrentDraftIsPublished(
    const TunerSnapshot& snapshot,
    const TunerTargetInput& draft,
    const std::optional<TunerTargetInput>& pending)
{
    return !pending.has_value() && snapshot.target.input == draft;
}

bool CurrentSelectionMatches(
    const TunerSnapshot& snapshot,
    const TunerTargetInput& draft,
    const std::optional<TunerTargetInput>& pending)
{
    return CurrentDraftIsPublished(snapshot, draft, pending)
        && snapshot.target.selection.has_value()
        && snapshot.target.selection->input == draft;
}

} // namespace

void HomePage::Draw(
    ITunerControl& tunerControl,
    const TunerSnapshot& snapshot,
    const GuiViewState& view,
    GuiPresentationSelection& selection,
    const Theme& theme,
    const Fonts& fonts,
    UiDialogs& dialogs)
{
    SynchronizeDraft(snapshot);
    if (view.page == GuiPage::ControllerConflict)
    {
        DrawControllerConflict(
            tunerControl, snapshot, view, theme, fonts);
        return;
    }
    if (IsHomePage(view.page))
    {
        DrawHome(
            tunerControl,
            snapshot,
            view,
            selection,
            theme,
            fonts,
            dialogs);
        return;
    }

    ImGui::TextColored(theme.statusWarning, "This page is not available yet.");
    ImGui::TextWrapped(
        "Use View > Legacy workflow to continue with the established "
        "project-guided interface.");
}

void HomePage::SynchronizeDraft(const TunerSnapshot& snapshot)
{
    if (!initialized_)
    {
        draft_ = snapshot.target.input;
        initialized_ = true;
    }

    if (pendingInput_)
    {
        if (snapshot.target.input == *pendingInput_)
            pendingInput_.reset();
        return;
    }

    if (snapshot.target.input != draft_)
        draft_ = snapshot.target.input;
}

void HomePage::SubmitEdit(ITunerControl& tunerControl)
{
    pendingInput_ = draft_;
    tunerControl.Submit(EditTunerTargetCommand{draft_});
}

void HomePage::DrawHome(
    ITunerControl& tunerControl,
    const TunerSnapshot& snapshot,
    const GuiViewState& view,
    GuiPresentationSelection& selection,
    const Theme& theme,
    const Fonts& fonts,
    UiDialogs& dialogs)
{
    const bool draftPublished = CurrentDraftIsPublished(
        snapshot, draft_, pendingInput_);
    const bool selectionCurrent = CurrentSelectionMatches(
        snapshot, draft_, pendingInput_);
    const bool editable =
        snapshot.tuningSession.evidence.endpointEditingAllowed
        && snapshot.tuningSession.evidence.operationIdle;
    const bool ready = view.claims.controllerReady && draftPublished;

    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float pageWidth = std::min(PageMaximumWidth, availableWidth);
    if (availableWidth > pageWidth)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX()
            + (availableWidth - pageWidth) * 0.5F);
    ImGui::BeginGroup();

    if (fonts.ui != nullptr)
        ImGui::PushFont(fonts.ui, 29.0F);
    ImGui::TextUnformatted("Ready to tune");
    if (fonts.ui != nullptr)
        ImGui::PopFont();
    ImGui::TextColored(
        theme.textDisabled,
        ready
            ? "The selected controller and target are verified and idle."
            : "Connect to one controller and check the target module.");
    ImGui::Spacing();
    ImGui::Spacing();

    bool inputChanged = false;
    const float cardWidth = (pageWidth - CardGap) * 0.5F;
    if (ImGui::BeginTable(
            "home_connection_cards",
            2,
            ImGuiTableFlags_SizingFixedFit,
            ImVec2(pageWidth, CardHeight)))
    {
        ImGui::TableSetupColumn(
            "Ethernet connection",
            ImGuiTableColumnFlags_WidthFixed,
            cardWidth);
        ImGui::TableSetupColumn(
            "Target module",
            ImGuiTableColumnFlags_WidthFixed,
            cardWidth);
        ImGui::TableNextColumn();

        BeginCard("##home_endpoint_card", ImVec2(-1.0F, CardHeight), theme);
        DrawCardTitle("Ethernet connection", fonts);
        ImGui::TextUnformatted("MVLC address");
        constexpr float actionWidth = 124.0F;
        ImGui::SetNextItemWidth(std::max(
            100.0F,
            ImGui::GetContentRegionAvail().x - actionWidth
                - ImGui::GetStyle().ItemSpacing.x));
        ImGui::BeginDisabled(!editable);
        inputChanged |= ImGui::InputText("##home_mvlc_host", &draft_.mvlcHost);
        ImGui::EndDisabled();
        const auto connectionValidation = ValidateTunerTargetInput(draft_);
        const bool connectEnabled =
            Allows(view.allowedActions, GuiAction::Connect)
            && connectionValidation.success
            && draftPublished
            && !inputChanged;
        ImGui::SameLine();
        if (DrawPrimaryButton(
                "Connect",
                connectEnabled,
                theme,
                ImVec2(actionWidth, 0.0F)))
        {
            tunerControl.Submit(SelectTunerTargetCommand{});
        }
        if (draft_.mvlcHost.empty())
        {
            ImGui::TextDisabled("Enter an MVLC hostname or IP address.");
        }
        else if (!connectionValidation.endpointValid)
        {
            DrawValidationMessage(
                connectionValidation.endpointMessage, false, theme);
        }
        else if (ready)
        {
            ImGui::TextColored(
                theme.statusGood,
                draft_.endpointKind == TunerTargetEndpointKind::SshBridge
                    ? "Connected through the selected SSH bridge."
                    : "Connected to MVLC over direct Ethernet.");
        }
        else
        {
            ImGui::TextDisabled(
                "The host and module address from the last successful Check "
                "are remembered.");
        }
        ImGui::EndChild();

        ImGui::TableNextColumn();
        BeginCard("##home_target_card", ImVec2(-1.0F, CardHeight), theme);
        DrawCardTitle("Target module", fonts);
        ImGui::TextUnformatted("Module address");
        ImGui::SetNextItemWidth(std::max(
            100.0F,
            ImGui::GetContentRegionAvail().x - actionWidth
                - ImGui::GetStyle().ItemSpacing.x));
        ImGui::BeginDisabled(!editable);
        inputChanged |= ImGui::InputText(
            "##home_module_address", &draft_.moduleAddress);
        ImGui::EndDisabled();
        const auto targetValidation = ValidateTunerTargetInput(draft_);
        const bool checkEnabled =
            Allows(view.allowedActions, GuiAction::Check)
            && selectionCurrent
            && !inputChanged;
        ImGui::SameLine();
        if (DrawPrimaryButton(
                snapshot.target.verification.inProgress
                    ? "Checking..."
                    : "Check",
                checkEnabled,
                theme,
                ImVec2(actionWidth, 0.0F)))
        {
            tunerControl.Submit(ProbeTunerTargetCommand{});
        }
        if (targetValidation.moduleAddressValid
            && targetValidation.normalizedModuleAddress)
        {
            if (ready && !inputChanged
                && view.targetModuleAddressA32.has_value())
            {
                const auto shorthand = GuiTargetAddressText(
                    *view.targetModuleAddressA32,
                    GuiTargetAddressDisplay::HomeMvmeShorthand);
                ImGui::TextColored(
                    theme.statusGood,
                    "MDPP-32 SCP FW2051 found at %s.",
                    shorthand.c_str());
            }
            else
            {
                ImGui::TextDisabled("Enter the address used in MVME.");
            }
        }
        else if (draft_.moduleAddress.empty())
        {
            ImGui::TextDisabled("Enter the address used in MVME.");
        }
        else
        {
            DrawValidationMessage(
                targetValidation.moduleAddressMessage, false, theme);
        }
        ImGui::EndChild();
        ImGui::EndTable();
    }

    ImGui::Spacing();
    const GuiAction sshAction = selection.sshSettingsExpanded
        ? GuiAction::CollapseSshSettings
        : GuiAction::ExpandSshSettings;
    const bool sshToggleEnabled = Allows(view.allowedActions, sshAction);
    ImGui::BeginDisabled(!sshToggleEnabled);
    const char* sshLabel = selection.sshSettingsExpanded
        ? ICON_FA_CHEVRON_DOWN "  Connect through SSH..."
        : ICON_FA_CHEVRON_RIGHT "  Connect through SSH...";
    if (ImGui::Selectable(sshLabel, false) && sshToggleEnabled)
        selection.sshSettingsExpanded = !selection.sshSettingsExpanded;
    ImGui::EndDisabled();

    if (selection.sshSettingsExpanded)
    {
        BeginCard("##home_ssh_card", ImVec2(pageWidth, 210.0F), theme);
        DrawCardTitle("SSH bridge", fonts);
        ImGui::BeginDisabled(!editable);
        const bool directSelected =
            draft_.endpointKind == TunerTargetEndpointKind::DirectEthernet;
        if (ImGui::RadioButton("Direct Ethernet", directSelected))
        {
            draft_.endpointKind = TunerTargetEndpointKind::DirectEthernet;
            inputChanged = true;
        }
        ImGui::SameLine(0.0F, 28.0F);
        if (ImGui::RadioButton("SSH bridge", !directSelected))
        {
            draft_.endpointKind = TunerTargetEndpointKind::SshBridge;
            inputChanged = true;
        }
        if (ImGui::BeginTable(
                "home_ssh_fields",
                2,
                ImGuiTableFlags_SizingStretchSame))
        {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("SSH destination");
            ImGui::SetNextItemWidth(-1.0F);
            inputChanged |= ImGui::InputText(
                "##home_ssh_destination", &draft_.sshDestination);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Remote bridge command");
            ImGui::SetNextItemWidth(-1.0F);
            inputChanged |= ImGui::InputText(
                "##home_bridge_command", &draft_.remoteBridgeCommand);
            ImGui::EndTable();
        }
        ImGui::EndDisabled();
        ImGui::TextDisabled(
            "SSH authentication uses a key or agent. Password prompts are "
            "disabled, and FIDGET stores no secrets.");
        const auto sshValidation = ValidateTunerTargetInput(draft_);
        if (draft_.endpointKind == TunerTargetEndpointKind::SshBridge
            && !sshValidation.endpointValid)
        {
            DrawValidationMessage(
                sshValidation.endpointMessage, false, theme);
        }
        ImGui::EndChild();
        ImGui::Spacing();
    }

    BeginCard("##home_workspace_card", ImVec2(pageWidth, 145.0F), theme);
    DrawCardTitle("MVME configuration (optional)", fonts);
    ImGui::TextWrapped(
        "Import a .vme file to use its module settings and create an "
        "updated copy after tuning.");
    const std::string workspaceName = workspacePath_.empty()
        ? std::string("Choose .vme file")
        : std::filesystem::path(workspacePath_).filename().string();
    if (ImGui::Button(
            (ICON_FA_FOLDER_OPEN "  " + workspaceName
             + "##home_workspace").c_str(),
            ImVec2(std::min(520.0F, pageWidth - 44.0F), 0.0F)))
    {
        if (const auto path = dialogs.OpenFile(MvmeWorkspaceFilter))
            workspacePath_ = *path;
    }
    if (!workspacePath_.empty())
    {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear##home_workspace"))
            workspacePath_.clear();
    }
    ImGui::EndChild();

    if (inputChanged)
        SubmitEdit(tunerControl);

    ImGui::Spacing();
    ImGui::Spacing();
    if (ready && !inputChanged)
    {
        ImGui::TextColored(theme.statusGood, ICON_FA_CHECK_CIRCLE);
        ImGui::SameLine();
        ImGui::PushTextWrapPos(
            ImGui::GetCursorPosX() + pageWidth - 220.0F);
        ImGui::TextWrapped(
            "No active acquisition was detected. MVME may remain open, "
            "but do not start a run or change hardware settings until "
            "tuning is finished.");
        ImGui::PopTextWrapPos();
    }
    else
    {
        ImGui::TextColored(theme.statusGood, ICON_FA_CHECK_CIRCLE);
        ImGui::SameLine();
        ImGui::TextDisabled(
            "FIDGET checks for active acquisition before taking control.");
    }

    if (selectionCurrent
        && snapshot.target.verification.result.outcome
            != TargetProbeOutcome::NotRun)
    {
        ImGui::Spacing();
        ImGui::TextColored(
            ProbeColor(snapshot.target.verification.result.outcome, theme),
            "%s",
            snapshot.target.verification.result.message.c_str());
    }
    if (!snapshot.statusMessages.empty()
        && snapshot.statusMessages.front().level
            != TunerStatusLevel::Information)
    {
        const auto& status = snapshot.statusMessages.front();
        ImGui::TextColored(
            StatusColor(status.level, theme), "%s", status.summary.c_str());
        if (!status.detail.empty())
            ImGui::TextWrapped("%s", status.detail.c_str());
    }

    const bool startEnabled =
        Allows(view.allowedActions, GuiAction::StartTuning)
        && draftPublished
        && !inputChanged
        && snapshot.target.sessionGate.outcome
            != TunerTargetSessionGateOutcome::ReadyForPreparation;
    const float startWidth = 176.0F;
    ImGui::SetCursorPosX(
        ImGui::GetCursorPosX() + std::max(
            0.0F, pageWidth - startWidth));
    if (DrawPrimaryButton(
            "Start tuning", startEnabled, theme, ImVec2(startWidth, 0.0F)))
    {
        tunerControl.Submit(OpenTunerTargetSessionCommand{});
    }

    ImGui::EndGroup();
}

void HomePage::DrawControllerConflict(
    ITunerControl& tunerControl,
    const TunerSnapshot& snapshot,
    const GuiViewState& view,
    const Theme& theme,
    const Fonts& fonts)
{
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float pageWidth = std::min(780.0F, availableWidth);
    if (availableWidth > pageWidth)
        ImGui::SetCursorPosX(ImGui::GetCursorPosX()
            + (availableWidth - pageWidth) * 0.5F);
    ImGui::BeginGroup();

    if (fonts.ui != nullptr)
        ImGui::PushFont(fonts.ui, 29.0F);
    ImGui::TextColored(theme.statusError, "Controller active elsewhere");
    if (fonts.ui != nullptr)
        ImGui::PopFont();
    ImGui::TextWrapped(
        "FIDGET detected active controller use and stopped before taking "
        "control.");
    ImGui::Spacing();

    BeginCard("##home_conflict_card", ImVec2(pageWidth, 205.0F), theme);
    if (view.claims.noControlTaken)
    {
        ImGui::TextColored(
            theme.statusGood,
            ICON_FA_CHECK_CIRCLE "  FIDGET did not take control.");
    }
    if (view.claims.noVmeOrModuleSettingWritesSent)
    {
        ImGui::TextColored(
            theme.statusGood,
            ICON_FA_CHECK_CIRCLE "  No hardware settings were changed.");
    }
    ImGui::Spacing();
    ImGui::TextWrapped(
        "MVME may remain open, but stop its run and keep all other control "
        "software idle before checking again.");
    ImGui::EndChild();

    ImGui::Spacing();
    const bool checkAgain = Allows(
        view.allowedActions, GuiAction::CheckAgain);
    if (DrawPrimaryButton("Check again", checkAgain, theme))
        tunerControl.Submit(ProbeTunerTargetCommand{});
    ImGui::SameLine();
    const bool returnHome = Allows(
        view.allowedActions, GuiAction::ReturnHome);
    ImGui::BeginDisabled(!returnHome);
    if (ImGui::Button("Return home") && returnHome)
    {
        // Re-submitting the same editable fields explicitly invalidates the
        // conflict probe while preserving everything the user entered.
        tunerControl.Submit(EditTunerTargetCommand{snapshot.target.input});
    }
    ImGui::EndDisabled();
    ImGui::EndGroup();
}

} // namespace fidget
