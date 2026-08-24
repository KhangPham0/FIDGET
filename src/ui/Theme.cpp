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

Theme DarkTheme()
{
    Theme t;
    t.name = "Dark";

    t.windowBackground = Hex(0x131316);
    t.panelBackground  = Hex(0x1B1B1F);
    t.childBackground  = Hex(0x18181C);
    t.popupBackground  = Hex(0x1F1F24);

    t.accent       = Hex(0x29BFAE);
    t.accentHover  = Hex(0x3BD9C8);
    t.accentActive = Hex(0x1FA396);

    t.highlight = Hex(0xE0A24A); // warm gold

    t.textPrimary  = Hex(0xE8E8EC);
    t.textDisabled = Hex(0x8B8B95);

    t.frame       = Hex(0x26262C);
    t.frameHover  = Hex(0x2E2E35);
    t.frameActive = Hex(0x35353D);

    t.border = Hex(0x2A2A31);

    t.statusGood    = Hex(0x58C97D);
    t.statusWarning = Hex(0xE5B454);
    t.statusError   = Hex(0xE25563);

    t.plotBackground = Hex(0x161619);
    t.plotGrid       = Hex(0x2A2A31, 0.50f);
    t.waveformLive   = t.accent;
    t.waveformTrail  = Hex(0xB9C2CC);
    t.referenceTrace = t.highlight;

    // Okabe-Ito palette: distinguishable under common color blindness.
    t.channelColors = {
        Hex(0xE69F00), // orange
        Hex(0xCC79A7), // purple
        Hex(0x56B4E9), // sky blue
        Hex(0x009E73), // green
        Hex(0xD55E00), // vermillion
        Hex(0xF0E442), // yellow
        Hex(0x0072B2), // blue
    };

    return t;
}

void ApplyTheme(const Theme& t)
{
    ImGuiStyle& style = ImGui::GetStyle();

    style.WindowRounding    = 0.0f;
    style.ChildRounding     = 4.0f;
    style.FrameRounding     = 4.0f;
    style.PopupRounding     = 4.0f;
    style.GrabRounding      = 4.0f;
    style.TabRounding       = 4.0f;
    style.ScrollbarRounding = 8.0f;

    style.WindowPadding    = ImVec2(10.0f, 10.0f);
    style.FramePadding     = ImVec2(8.0f, 5.0f);
    style.ItemSpacing      = ImVec2(8.0f, 6.0f);
    style.WindowBorderSize = 0.0f;
    style.FrameBorderSize  = 0.0f;

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

    // Panel names read in teal by default; the focused panel switches to the
    // gold highlight, so the active panel stands out among the teal tabs.
    auto mix = [](const ImVec4& base, const ImVec4& tint, float amount) {
        return ImVec4((1.0f - amount) * base.x + amount * tint.x,
                      (1.0f - amount) * base.y + amount * tint.y,
                      (1.0f - amount) * base.z + amount * tint.z, 1.0f);
    };
    ImVec4 tealTab      = mix(t.panelBackground, t.accent, 0.28f);
    ImVec4 tealTabHover = mix(t.panelBackground, t.accent, 0.42f);
    ImVec4 goldTab      = mix(t.panelBackground, t.highlight, 0.30f);
    colors[ImGuiCol_Tab]                       = tealTab;
    colors[ImGuiCol_TabHovered]                = tealTabHover;
    colors[ImGuiCol_TabSelected]               = goldTab;
    colors[ImGuiCol_TabSelectedOverline]       = t.highlight;
    colors[ImGuiCol_TabDimmed]                 = tealTab;
    colors[ImGuiCol_TabDimmedSelected]         = tealTab;
    colors[ImGuiCol_TabDimmedSelectedOverline] = t.accent;

    colors[ImGuiCol_DockingPreview] = ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.55f);
    colors[ImGuiCol_DockingEmptyBg] = t.windowBackground;

    colors[ImGuiCol_TableHeaderBg]     = t.frame;
    colors[ImGuiCol_TableBorderStrong] = t.border;
    colors[ImGuiCol_TableBorderLight]  = t.border;
    colors[ImGuiCol_TableRowBg]        = ImVec4(0, 0, 0, 0);
    colors[ImGuiCol_TableRowBgAlt]     = ImVec4(1, 1, 1, 0.02f);

    colors[ImGuiCol_TextSelectedBg]        = ImVec4(t.accent.x, t.accent.y, t.accent.z, 0.35f);
    colors[ImGuiCol_DragDropTarget]        = t.accentHover;
    colors[ImGuiCol_NavCursor]             = t.accent;
    colors[ImGuiCol_NavWindowingHighlight] = t.accent;
    colors[ImGuiCol_ModalWindowDimBg]      = ImVec4(0, 0, 0, 0.50f);

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
