#ifndef FIDGET_UI_FONTS_H
#define FIDGET_UI_FONTS_H

#include "imgui.h"

namespace fidget {

// The fonts FIDGET uses, embedded in the binary so no files are needed at
// runtime. Sizes are dynamic: pass a size to ImGui::PushFont when a
// different size is needed.
//
// FIDGET uses one typeface throughout: Cascadia Code Bold. It is
// monospaced, so it serves both the interface text and the numeric
// columns (register values, sample counts, rates) where digits must align.
struct Fonts
{
    ImFont* ui = nullptr;   // interface text (the default font)
    ImFont* mono = nullptr; // numbers and tables (same face, aligned digits)
};

// Loads the embedded fonts into the ImGui font atlas. Call once at startup,
// after ImGui::CreateContext.
Fonts LoadFonts();

} // namespace fidget

#endif
