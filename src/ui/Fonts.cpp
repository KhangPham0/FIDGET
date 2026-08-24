#include "Fonts.h"

// Generated during the build from Dear ImGui's vendored Karla, Roboto, and
// Cousine font files. Their license notices ship with that vendored source.
#include "ui/fonts/CousineRegular.h"
#include "ui/fonts/KarlaRegular.h"
#include "ui/fonts/RobotoMedium.h"

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
    fonts.ui = io.Fonts->AddFontFromMemoryCompressedTTF(
        KarlaRegular_compressed_data,
        static_cast<int>(KarlaRegular_compressed_size));

    // Icons merge into the font, so ICON_FA_* strings render inline. The
    // atlas keeps the range pointer: it must outlive the frame.
    static const ImWchar kIconRanges[] = { ICON_MIN_FA, ICON_MAX_FA, 0 };
    ImFontConfig iconConfig;
    iconConfig.MergeMode = true;
    iconConfig.PixelSnapH = true;
    iconConfig.FontDataOwnedByAtlas = false; // the array is a global
    io.Fonts->AddFontFromMemoryTTF(fa_solid_900_ttf, fa_solid_900_ttf_len, 0.0f,
                                   &iconConfig, kIconRanges);

    fonts.heading = io.Fonts->AddFontFromMemoryCompressedTTF(
        RobotoMedium_compressed_data,
        static_cast<int>(RobotoMedium_compressed_size));
    fonts.mono = io.Fonts->AddFontFromMemoryCompressedTTF(
        CousineRegular_compressed_data,
        static_cast<int>(CousineRegular_compressed_size));
    io.FontDefault = fonts.ui;

    return fonts;
}

} // namespace fidget
