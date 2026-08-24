#include "Fonts.h"

// Cascadia Code Bold (Nerd Font variant), the single typeface used
// throughout. Generated with imgui's binary_to_compressed_c tool (-base85).
#include "fonts/cascadia_bold.h"

// Font Awesome 5 Free Solid (raw TTF array) and its icon codepoints.
#include "fonts/IconsFontAwesome5.h"
#include "fonts/fa_solid.h"

namespace fidget {

Fonts LoadFonts()
{
    ImGuiIO& io = ImGui::GetIO();

    // The default rendered size; PushFont can override it per use.
    ImGui::GetStyle().FontSizeBase = 16.0f;

    Fonts fonts;
    fonts.ui = io.Fonts->AddFontFromMemoryCompressedBase85TTF(CascadiaBold_compressed_data_base85);

    // Icons merge into the font, so ICON_FA_* strings render inline. The
    // atlas keeps the range pointer: it must outlive the frame.
    static const ImWchar kIconRanges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
    ImFontConfig iconConfig;
    iconConfig.MergeMode = true;
    iconConfig.PixelSnapH = true;
    iconConfig.FontDataOwnedByAtlas = false; // the array is a global
    io.Fonts->AddFontFromMemoryTTF(fa_solid_900_ttf, fa_solid_900_ttf_len, 0.0f,
                                   &iconConfig, kIconRanges);

    // Cascadia is monospaced, so the same face aligns the numeric columns.
    fonts.mono = fonts.ui;
    io.FontDefault = fonts.ui;

    return fonts;
}

} // namespace fidget
