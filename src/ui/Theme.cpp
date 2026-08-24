#include "Theme.h"

#include "implot.h"

namespace fidget {

// Builds an ImVec4 from a 0xRRGGBB hex value, e.g. Hex(0x131316).
static ImVec4 Hex(unsigned int rgb, float alpha = 1.0f)
{
    float r = ((rgb >> 16) & 0xFF) / 255.0f;
    float g = ((rgb >> 8) & 0xFF) / 255.0f;
    float b = (rgb & 0xFF) / 255.0f;
    return ImVec4(r, g, b, alpha);
}

Theme LightTheme()
{
    Theme t;
    t.name = "FIDGET Light";

    t.windowBackground = Hex(0xF6F8FB);
    t.panelBackground  = Hex(0xFFFFFF);
    t.childBackground  = Hex(0xFFFFFF);
    t.popupBackground  = Hex(0xFFFFFF);

    t.accent       = Hex(0x26374B);
    t.accentHover  = Hex(0x334B66);
    t.accentActive = Hex(0x1E2C3D);
    t.textOnAccent = Hex(0xFFFFFF);

    t.highlight = Hex(0xD97B29);

    t.textPrimary  = Hex(0x17202B);
    t.textDisabled = Hex(0x667085);

    t.frame       = Hex(0xFFFFFF);
    t.frameHover  = Hex(0xEEF2F6);
    t.frameActive = Hex(0xE4EAF1);

    t.border = Hex(0xD8DEE8);

    t.statusGood    = Hex(0x16824A);
    t.statusWarning = Hex(0xD97B29);
    t.statusError   = Hex(0xC43D32);

    t.plotBackground = Hex(0xFFFFFF);
    t.plotGrid       = Hex(0xD8DEE8, 0.75f);
    t.waveformLive   = Hex(0x2878C8);
    t.waveformTrail  = Hex(0x8CB8E2);
    t.referenceTrace = t.highlight;

    // The first colors are the approved waveform and comparison tokens. The
    // remaining colors extend the cycle while preserving visible contrast.
    t.channelColors = {
        Hex(0x2878C8),
        Hex(0xD97B29),
        Hex(0x7A5ABD),
        Hex(0x258A85),
        Hex(0x16824A),
        Hex(0xC43D32),
        Hex(0x667085),
    };

    return t;
}

void ApplyTheme(const Theme& t)
{
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowRounding    = 8.0f;
    style.ChildRounding     = 8.0f;
    style.FrameRounding     = 6.0f;
    style.PopupRounding     = 8.0f;
    style.GrabRounding      = 6.0f;
    style.TabRounding       = 6.0f;
    style.ScrollbarRounding = 8.0f;

    style.WindowPadding    = ImVec2(10.0f, 10.0f);
    style.FramePadding     = ImVec2(8.0f, 5.0f);
    style.ItemSpacing      = ImVec2(8.0f, 6.0f);
    style.WindowBorderSize = 1.0f;
    style.FrameBorderSize  = 1.0f;

    ImVec4* colors = style.Colors;

    colors[ImGuiCol_Text]         = t.textPrimary;
    colors[ImGuiCol_TextDisabled] = t.textDisabled;

    colors[ImGuiCol_WindowBg] = t.panelBackground;
    colors[ImGuiCol_ChildBg]  = t.childBackground;
    colors[ImGuiCol_PopupBg]  = t.popupBackground;

    colors[ImGuiCol_Border]       = t.border;
    colors[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);

    colors[ImGuiCol_FrameBg]        = t.frame;
    colors[ImGuiCol_FrameBgHovered] = t.frameHover;
    colors[ImGuiCol_FrameBgActive]  = t.frameActive;

    colors[ImGuiCol_TitleBg]          = t.windowBackground;
    colors[ImGuiCol_TitleBgActive]    = t.windowBackground;
    colors[ImGuiCol_TitleBgCollapsed] = t.windowBackground;
    colors[ImGuiCol_MenuBarBg]        = t.windowBackground;

    colors[ImGuiCol_ScrollbarBg]          = t.panelBackground;
    colors[ImGuiCol_ScrollbarGrab]        = t.frame;
    colors[ImGuiCol_ScrollbarGrabHovered] = t.frameHover;
    colors[ImGuiCol_ScrollbarGrabActive]  = t.frameActive;

    colors[ImGuiCol_CheckMark]        = t.accent;
    colors[ImGuiCol_SliderGrab]       = t.accent;
    colors[ImGuiCol_SliderGrabActive] = t.accentHover;

    colors[ImGuiCol_Button]        = t.frame;
    colors[ImGuiCol_ButtonHovered] = t.frameHover;
    colors[ImGuiCol_ButtonActive]  = t.frameActive;

    colors[ImGuiCol_Header]        = t.frame;
    colors[ImGuiCol_HeaderHovered] = t.frameHover;
    colors[ImGuiCol_HeaderActive]  = t.frameActive;

    colors[ImGuiCol_Separator]        = t.border;
    colors[ImGuiCol_SeparatorHovered] = t.accent;
    colors[ImGuiCol_SeparatorActive]  = t.accentActive;

    colors[ImGuiCol_ResizeGrip]        = t.frame;
    colors[ImGuiCol_ResizeGripHovered] = t.accentHover;
    colors[ImGuiCol_ResizeGripActive]  = t.accentActive;

    // Legacy dock tabs remain quiet under the new shell. The selected tab uses
    // a pale navy tint and an explicit overline.
    auto mix = [](const ImVec4& base, const ImVec4& tint, float amount) {
        return ImVec4((1.0f - amount) * base.x + amount * tint.x,
                      (1.0f - amount) * base.y + amount * tint.y,
                      (1.0f - amount) * base.z + amount * tint.z, 1.0f);
    };
    ImVec4 quietTab    = mix(t.panelBackground, t.accent, 0.06f);
    ImVec4 hoveredTab  = mix(t.panelBackground, t.accent, 0.12f);
    ImVec4 selectedTab = mix(t.panelBackground, t.accent, 0.16f);
    colors[ImGuiCol_Tab]                       = quietTab;
    colors[ImGuiCol_TabHovered]                = hoveredTab;
    colors[ImGuiCol_TabSelected]               = selectedTab;
    colors[ImGuiCol_TabSelectedOverline]       = t.accent;
    colors[ImGuiCol_TabDimmed]                 = quietTab;
    colors[ImGuiCol_TabDimmedSelected]         = quietTab;
    colors[ImGuiCol_TabDimmedSelectedOverline] = t.accent;

    colors[ImGuiCol_DockingPreview] = ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.55f);
    colors[ImGuiCol_DockingEmptyBg] = t.windowBackground;

    colors[ImGuiCol_TableHeaderBg]     = t.frame;
    colors[ImGuiCol_TableBorderStrong] = t.border;
    colors[ImGuiCol_TableBorderLight]  = t.border;
    colors[ImGuiCol_TableRowBg]        = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_TableRowBgAlt]     = Hex(0xF6F8FB);

    colors[ImGuiCol_TextSelectedBg]        = ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.35f);
    colors[ImGuiCol_DragDropTarget]        = t.accentHover;
    colors[ImGuiCol_NavCursor]             = t.accent;
    colors[ImGuiCol_NavWindowingHighlight] = t.accent;
    colors[ImGuiCol_ModalWindowDimBg]      = ImVec4(0.10f, 0.14f, 0.20f, 0.28f);

    ImPlotStyle& plotStyle = ImPlot::GetStyle();
    ImVec4* plotColors = plotStyle.Colors;

    plotColors[ImPlotCol_FrameBg]      = ImVec4(0, 0, 0, 0);
    plotColors[ImPlotCol_PlotBg]       = t.plotBackground;
    plotColors[ImPlotCol_PlotBorder]   = t.border;
    plotColors[ImPlotCol_AxisGrid]     = t.plotGrid;
    plotColors[ImPlotCol_AxisText]     = t.textDisabled;
    plotColors[ImPlotCol_LegendBg]     = ImVec4(t.popupBackground.x, t.popupBackground.y, t.popupBackground.z, 0.85f);
    plotColors[ImPlotCol_LegendBorder] = t.border;
    plotColors[ImPlotCol_TitleText]    = t.textPrimary;
    plotColors[ImPlotCol_InlayText]    = t.textPrimary;
    plotColors[ImPlotCol_Selection]    = t.accent;
    plotColors[ImPlotCol_Crosshairs]   = t.textDisabled;

    plotStyle.MinorGridSize = ImVec2(1.0f, 1.0f);
    plotStyle.MajorGridSize = ImVec2(1.0f, 1.0f);

    // A little breathing room when auto-fitting, so a tall waveform does not
    // touch the top edge.
    plotStyle.FitPadding = ImVec2(0.015f, 0.06f);
}

} // namespace fidget
