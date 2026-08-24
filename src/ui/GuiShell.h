#ifndef FIDGET_UI_GUI_SHELL_H
#define FIDGET_UI_GUI_SHELL_H

#include "presentation/GuiPresentation.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"

namespace fidget {

struct GuiShellResult
{
    GuiDrawer requestedDrawer = GuiDrawer::Count;
    bool closeDrawer = false;
};

// Draws the fixed presentation shell and returns only UI-local drawer intent.
// The shell receives no backend object and derives every label and enabled
// drawer action from the immutable GUI view state.
[[nodiscard]] float DrawGuiShellHeader(
    const GuiViewState& view,
    const Theme& theme,
    const Fonts& fonts,
    GuiShellResult& result);

void DrawGuiShellDrawer(
    const GuiViewState& view,
    float headerHeight,
    const Theme& theme,
    const Fonts& fonts,
    GuiShellResult& result);

} // namespace fidget

#endif
