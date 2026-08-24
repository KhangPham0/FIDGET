#ifndef FIDGET_UI_FONTS_H
#define FIDGET_UI_FONTS_H

#include "imgui.h"

namespace fidget {

// The fonts FIDGET uses, embedded in the binary so no files are needed at
// runtime. Sizes are dynamic: pass a size to ImGui::PushFont when a
// different size is needed.
//
// The approved shell uses a readable proportional face for interface text,
// a medium-weight face for headings, and a monospaced face only where values
// must align. All three are generated into C++ data from vendored fonts at
// build time, so startup never reads a font outside the application binary.
struct Fonts
{
    ImFont* ui = nullptr;
    ImFont* heading = nullptr;
    ImFont* mono = nullptr;
};

// Loads the embedded fonts into the ImGui font atlas. Call once at startup,
// after ImGui::CreateContext.
Fonts LoadFonts();

} // namespace fidget

#endif
