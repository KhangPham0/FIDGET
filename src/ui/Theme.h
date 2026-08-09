#ifndef FIDGET_UI_THEME_H
#define FIDGET_UI_THEME_H

#include <cstddef>
#include <vector>

#include "imgui.h"

namespace fidget {

// Every color that defines FIDGET's appearance, kept as plain data so that
// alternative themes (light, university colors, ...) can be added later
// without touching any drawing code.
struct Theme
{
    const char* name = "";

    // Base surfaces
    ImVec4 windowBackground;
    ImVec4 panelBackground;
    ImVec4 childBackground;
    ImVec4 popupBackground;

    // The accent color, used sparingly: selection, the primary next action,
    // focus, and the latest live waveform trace.
    ImVec4 accent;
    ImVec4 accentHover;
    ImVec4 accentActive;

    // A second highlight, distinct from the accent, marking the focused
    // panel and the frozen reference trace.
    ImVec4 highlight;

    // Text
    ImVec4 textPrimary;
    ImVec4 textDisabled;

    // Widget surfaces (inputs, buttons, headers)
    ImVec4 frame;
    ImVec4 frameHover;
    ImVec4 frameActive;

    // Borders and separators
    ImVec4 border;

    // Status colors. Good marks verified and ready states, warning marks
    // attention states such as a stale snapshot or a pending confirmation,
    // and error marks blocked states such as a foreign DAQ or a failed
    // cleanup. Hardware-writing buttons use the warning color.
    ImVec4 statusGood;
    ImVec4 statusWarning;
    ImVec4 statusError;

    // Plot area
    ImVec4 plotBackground;
    ImVec4 plotGrid;
    ImVec4 waveformLive;  // the latest trace, full strength
    ImVec4 waveformTrail; // older trails, drawn with an age-based alpha ramp
    ImVec4 referenceTrace;

    // Colors for waveforms of different channels shown together (cycled
    // when there are more channels than colors).
    std::vector<ImVec4> channelColors;

    ImVec4 ChannelColor(std::size_t index) const
    {
        return channelColors.empty()
                   ? waveformLive
                   : channelColors[index % channelColors.size()];
    }
};

// The default FIDGET theme, shared with GIGGLE V3.
Theme DarkTheme();

// Writes a theme into the global ImGui and ImPlot styles.
void ApplyTheme(const Theme& theme);

} // namespace fidget

#endif
