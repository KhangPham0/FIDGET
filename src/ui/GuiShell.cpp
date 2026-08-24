#include "ui/GuiShell.h"

#include <algorithm>

#include "imgui.h"

namespace fidget {
namespace {

constexpr float ShellHeaderHeight = 62.0F;
constexpr float HeaderHorizontalPadding = 24.0F;
constexpr float HeaderItemSpacing = 22.0F;
constexpr float StatusDotRadius = 5.0F;
constexpr float DrawerWidth = 372.0F;
constexpr float DrawerMargin = 16.0F;

bool IsHomePage(const GuiPage page) noexcept
{
    return page == GuiPage::HomeDisconnected
        || page == GuiPage::HomeReady
        || page == GuiPage::HomeSshExpanded;
}

bool IsTunePage(const GuiPage page) noexcept
{
    switch (page)
    {
    case GuiPage::Preparing:
    case GuiPage::Goal:
    case GuiPage::Group:
    case GuiPage::AutomaticLearnSignal:
    case GuiPage::AutomaticEnergy:
    case GuiPage::AutomaticTiming:
    case GuiPage::ManualEnergy:
    case GuiPage::ManualTiming:
    case GuiPage::CompareChannels:
    case GuiPage::GroupResult:
    case GuiPage::Restoring:
    case GuiPage::NextGroup:
    case GuiPage::Finished:
        return true;
    default:
        return false;
    }
}

ImVec4 StatusColor(
    const GuiHeaderConnectionStatus status,
    const Theme& theme) noexcept
{
    switch (status)
    {
    case GuiHeaderConnectionStatus::Connected:
        return theme.statusGood;
    case GuiHeaderConnectionStatus::HasControl:
    case GuiHeaderConnectionStatus::RecoveryInProgress:
        return theme.waveformLive;
    case GuiHeaderConnectionStatus::ControllerConflict:
    case GuiHeaderConnectionStatus::RecoveryBlocked:
        return theme.statusError;
    case GuiHeaderConnectionStatus::OwnershipUncertain:
        return theme.statusWarning;
    case GuiHeaderConnectionStatus::Unknown:
    case GuiHeaderConnectionStatus::NotConnected:
    case GuiHeaderConnectionStatus::Count:
        return theme.textDisabled;
    }
    return theme.textDisabled;
}

const char* DrawerTitle(const GuiDrawer drawer) noexcept
{
    switch (drawer)
    {
    case GuiDrawer::Details:
        return "Details";
    case GuiDrawer::Logs:
        return "Logs";
    case GuiDrawer::Help:
        return "Help";
    case GuiDrawer::SettingChanges:
        return "Setting changes";
    case GuiDrawer::None:
    case GuiDrawer::Count:
        return "";
    }
    return "";
}

void DrawNavigationLabel(
    const char* label,
    const bool selected,
    const Theme& theme)
{
    const ImVec4 color = selected ? theme.accent : theme.textDisabled;
    ImGui::TextColored(color, "%s", label);
    if (selected)
    {
        const ImVec2 minimum = ImGui::GetItemRectMin();
        const ImVec2 maximum = ImGui::GetItemRectMax();
        ImGui::GetWindowDrawList()->AddLine(
            ImVec2(minimum.x, maximum.y + 3.0F),
            ImVec2(maximum.x, maximum.y + 3.0F),
            ImGui::ColorConvertFloat4ToU32(theme.accent),
            2.0F);
    }
}

bool DrawDrawerButton(
    const char* label,
    const GuiAction action,
    const GuiDrawer drawer,
    const GuiViewState& view,
    const Theme& theme)
{
    const bool allowed = Allows(view.allowedActions, action);
    const bool selected = view.drawer == drawer;

    ImGui::PushStyleColor(
        ImGuiCol_Button,
        selected ? theme.frameActive : theme.panelBackground);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.frameHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.frameActive);
    ImGui::PushStyleColor(
        ImGuiCol_Text,
        allowed ? theme.textPrimary : theme.textDisabled);
    ImGui::BeginDisabled(!allowed);
    const bool pressed = ImGui::Button(label);
    ImGui::EndDisabled();
    ImGui::PopStyleColor(4);
    return pressed && allowed;
}

} // namespace

float DrawGuiShellHeader(
    const GuiViewState& view,
    const Theme& theme,
    const Fonts& fonts,
    GuiShellResult& result)
{
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float headerHeight = std::max(
        ShellHeaderHeight, ImGui::GetFrameHeight() + 28.0F);
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(
        ImVec2(viewport->WorkSize.x, headerHeight));
    ImGui::SetNextWindowViewport(viewport->ID);

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration
        | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoDocking
        | ImGuiWindowFlags_NoSavedSettings
        | ImGuiWindowFlags_NoBringToFrontOnFocus;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme.panelBackground);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0F);
    ImGui::PushStyleVar(
        ImGuiStyleVar_WindowPadding,
        ImVec2(HeaderHorizontalPadding, 16.0F));
    ImGui::Begin("##fidget_shell_header", nullptr, flags);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);

    if (fonts.heading != nullptr)
        ImGui::PushFont(fonts.heading, 19.0F);
    ImGui::TextColored(theme.accent, "FIDGET");
    if (fonts.heading != nullptr)
        ImGui::PopFont();

    ImGui::SameLine(0.0F, 34.0F);
    DrawNavigationLabel("Home", IsHomePage(view.page), theme);
    ImGui::SameLine(0.0F, HeaderItemSpacing);
    DrawNavigationLabel("Tune", IsTunePage(view.page), theme);

    const char* statusText = GuiHeaderConnectionStatusText(
        view.headerConnection);
    const ImVec2 statusTextSize = ImGui::CalcTextSize(statusText);
    const float statusWidth = StatusDotRadius * 2.0F + 10.0F
        + statusTextSize.x;
    const float centeredStatusX = std::max(
        ImGui::GetCursorPosX(),
        (viewport->WorkSize.x - statusWidth) * 0.5F);
    ImGui::SameLine(centeredStatusX);

    const ImVec2 dotCenter(
        ImGui::GetCursorScreenPos().x + StatusDotRadius,
        ImGui::GetCursorScreenPos().y + ImGui::GetTextLineHeight() * 0.5F);
    ImGui::GetWindowDrawList()->AddCircleFilled(
        dotCenter,
        StatusDotRadius,
        ImGui::ColorConvertFloat4ToU32(
            StatusColor(view.headerConnection, theme)));
    ImGui::Dummy(ImVec2(StatusDotRadius * 2.0F, ImGui::GetTextLineHeight()));
    ImGui::SameLine(0.0F, 10.0F);
    ImGui::TextUnformatted(statusText);

    const float detailsWidth = ImGui::CalcTextSize("Details").x
        + ImGui::GetStyle().FramePadding.x * 2.0F;
    const float logsWidth = ImGui::CalcTextSize("Logs").x
        + ImGui::GetStyle().FramePadding.x * 2.0F;
    const float helpWidth = ImGui::CalcTextSize("Help").x
        + ImGui::GetStyle().FramePadding.x * 2.0F;
    const float rightWidth = detailsWidth + logsWidth + helpWidth
        + ImGui::GetStyle().ItemSpacing.x * 2.0F;
    ImGui::SameLine(std::max(
        ImGui::GetCursorPosX(),
        viewport->WorkSize.x - HeaderHorizontalPadding - rightWidth));

    if (DrawDrawerButton(
            "Details", GuiAction::OpenDetails, GuiDrawer::Details,
            view, theme))
    {
        result.requestedDrawer = GuiDrawer::Details;
    }
    ImGui::SameLine();
    if (DrawDrawerButton(
            "Logs", GuiAction::OpenLogs, GuiDrawer::Logs,
            view, theme))
    {
        result.requestedDrawer = GuiDrawer::Logs;
    }
    ImGui::SameLine();
    if (DrawDrawerButton(
            "Help", GuiAction::OpenHelp, GuiDrawer::Help,
            view, theme))
    {
        result.requestedDrawer = GuiDrawer::Help;
    }

    const ImVec2 windowMinimum = ImGui::GetWindowPos();
    const ImVec2 windowSize = ImGui::GetWindowSize();
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(windowMinimum.x, windowMinimum.y + windowSize.y - 1.0F),
        ImVec2(
            windowMinimum.x + windowSize.x,
            windowMinimum.y + windowSize.y - 1.0F),
        ImGui::ColorConvertFloat4ToU32(theme.border));

    ImGui::End();
    return headerHeight;
}

void DrawGuiShellDrawer(
    const GuiViewState& view,
    const float headerHeight,
    const Theme& theme,
    const Fonts& fonts,
    GuiShellResult& result)
{
    if (view.drawer == GuiDrawer::None || view.drawer == GuiDrawer::Count)
        return;

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float availableWidth = std::max(
        0.0F,
        viewport->WorkSize.x - DrawerMargin * 2.0F);
    const float width = std::min(DrawerWidth, availableWidth);
    ImGui::SetNextWindowPos(ImVec2(
        viewport->WorkPos.x + viewport->WorkSize.x - width - DrawerMargin,
        viewport->WorkPos.y + headerHeight + DrawerMargin));
    ImGui::SetNextWindowSize(ImVec2(
        width,
        std::max(
            0.0F,
            viewport->WorkSize.y - headerHeight - DrawerMargin * 2.0F)));
    ImGui::SetNextWindowViewport(viewport->ID);

    constexpr ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration
        | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize
        | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings;

    ImGui::PushStyleColor(ImGuiCol_WindowBg, theme.panelBackground);
    ImGui::PushStyleColor(ImGuiCol_Border, theme.border);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0F);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(22.0F, 20.0F));
    ImGui::Begin("##fidget_shell_drawer", nullptr, flags);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);

    if (fonts.heading != nullptr)
        ImGui::PushFont(fonts.heading, 18.0F);
    ImGui::TextUnformatted(DrawerTitle(view.drawer));
    if (fonts.heading != nullptr)
        ImGui::PopFont();

    const char* closeLabel = "Close";
    const float closeWidth = ImGui::CalcTextSize(closeLabel).x
        + ImGui::GetStyle().FramePadding.x * 2.0F;
    ImGui::SameLine(
        std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionAvail().x
            + ImGui::GetCursorPosX() - closeWidth));
    const bool closeAllowed = Allows(
        view.allowedActions, GuiAction::CloseDrawer);
    ImGui::BeginDisabled(!closeAllowed);
    if (ImGui::Button(closeLabel) && closeAllowed)
        result.closeDrawer = true;
    ImGui::EndDisabled();
    ImGui::Separator();

    ImGui::End();
}

} // namespace fidget
