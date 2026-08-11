#include "ui/ConfigurationStage.h"

#include "core/ScpRegistry.h"

#include "imgui.h"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>

namespace fidget {
namespace {

ImVec4 CaptureColor(
    const Fw2051ScpConfigurationSnapshot& capture,
    const Theme& theme)
{
    switch (capture.state)
    {
    case ScpConfigurationState::NotRun:
        return theme.textDisabled;
    case ScpConfigurationState::Reading:
        return theme.accent;
    case ScpConfigurationState::Complete:
        return theme.statusGood;
    case ScpConfigurationState::Failed:
        return theme.statusError;
    }
    return theme.textDisabled;
}

std::string FormatValue(
    const std::optional<std::uint32_t>& value,
    const bool hexadecimal)
{
    if (!value)
    {
        return "-";
    }
    if (!hexadecimal)
    {
        return std::to_string(*value);
    }

    char text[16]{};
    std::snprintf(
        text,
        sizeof(text),
        *value > 0xFFFFU ? "0x%08X" : "0x%04X",
        static_cast<unsigned>(*value));
    return text;
}

void DrawComparisonRow(
    const char* scope,
    const std::optional<std::uint16_t>& registerOffset,
    const char* setting,
    const std::optional<std::uint32_t>& profileValue,
    const std::optional<std::uint32_t>& capturedValue,
    const bool hexadecimal,
    const Theme& theme)
{
    const bool different = profileValue && capturedValue &&
        *profileValue != *capturedValue;
    ImGui::TableNextRow();
    if (different)
    {
        ImVec4 background = theme.statusWarning;
        background.w = 0.20F;
        ImGui::TableSetBgColor(
            ImGuiTableBgTarget_RowBg0,
            ImGui::ColorConvertFloat4ToU32(background));
    }

    ImGui::TableNextColumn();
    ImGui::TextUnformatted(scope);
    ImGui::TableNextColumn();
    if (registerOffset)
    {
        ImGui::Text("0x%04X", static_cast<unsigned>(*registerOffset));
    }
    else
    {
        ImGui::TextDisabled("metadata");
    }
    ImGui::TableNextColumn();
    ImGui::TextUnformatted(setting);
    ImGui::TableNextColumn();
    const auto profileText = FormatValue(profileValue, hexadecimal);
    ImGui::TextUnformatted(profileText.c_str());
    ImGui::TableNextColumn();
    const auto capturedText = FormatValue(capturedValue, hexadecimal);
    if (different)
    {
        ImGui::TextColored(
            theme.statusWarning, "%s", capturedText.c_str());
    }
    else
    {
        ImGui::TextUnformatted(capturedText.c_str());
    }
}

std::optional<std::uint32_t> ValueIf(
    const bool available,
    const std::uint32_t value)
{
    return available
        ? std::optional<std::uint32_t>{value}
        : std::nullopt;
}

} // namespace

void ConfigurationStage::Draw(
    ITunerControl& tunerControl,
    const TunerSnapshot& snapshot,
    const Theme& theme)
{
    ImGui::TextUnformatted("Capture and compare");
    ImGui::TextWrapped(
        "Captures 141 profile values from the FW2051 register map. This "
        "operation writes only the channel-quad selector at 0x6100: quads "
        "0 through 7 while reading, followed by a final parking write of "
        "quad 0. It does not change a detector parameter and never runs "
        "automatically.");
    ImGui::Spacing();

    const bool sessionOpen =
        snapshot.ownership == GuidedTunerOwnershipState::SessionOpen;
    const bool captureRunning = snapshot.activeOperation ==
        GuidedTunerOperation::ConfigurationCapture;
    const bool mayCapture = sessionOpen && snapshot.targetSupported &&
        !captureRunning;
    ImGui::BeginDisabled(!mayCapture);
    if (ImGui::Button("Capture 141 values (selector writes only)"))
    {
        tunerControl.Submit(CaptureConfigurationCommand{});
    }
    ImGui::EndDisabled();

    if (sessionOpen ||
        snapshot.ownership == GuidedTunerOwnershipState::OwnershipLost)
    {
        ImGui::SameLine();
        if (ImGui::Button("Release session"))
        {
            tunerControl.Submit(ReleaseSessionCommand{});
        }
    }
    if (!sessionOpen)
    {
        ImGui::TextDisabled(
            "Open the ownership session before capturing configuration.");
    }

    const auto& capture = snapshot.configurationCapture;
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(
        CaptureColor(capture, theme), "%s", capture.message.c_str());

    if (capture.state != ScpConfigurationState::NotRun &&
        !snapshot.configurationFresh)
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(
            theme.statusWarning.x,
            theme.statusWarning.y,
            theme.statusWarning.z,
            0.16F));
        ImGui::BeginChild(
            "stale_capture_warning", ImVec2(0.0F, 58.0F), true);
        ImGui::TextColored(theme.statusWarning, "STALE CONFIGURATION");
        ImGui::TextWrapped(
            "This capture is incomplete or no longer belongs to the "
            "current live target. Do not use it as current hardware state.");
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    if (!capture.selectorWrites.empty())
    {
        ImGui::SeparatorText("Selector-write log");
        if (ImGui::BeginTable(
                "selector_write_log",
                4,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Register");
            ImGui::TableSetupColumn("Value");
            ImGui::TableSetupColumn("Purpose");
            ImGui::TableSetupColumn("Result");
            ImGui::TableHeadersRow();
            for (const auto& write : capture.selectorWrites)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text(
                    "0x%04X", static_cast<unsigned>(write.registerOffset));
                ImGui::TableNextColumn();
                ImGui::Text("%u", static_cast<unsigned>(write.value));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(
                    write.parkingWrite ? "park at quad 0" : "select bank");
                ImGui::TableNextColumn();
                ImGui::TextColored(
                    write.success ? theme.statusGood : theme.statusError,
                    "%s",
                    write.success ? "completed" : write.message.c_str());
            }
            ImGui::EndTable();
        }
    }

    if (snapshot.configurationComparison.comparable)
    {
        const bool identical =
            snapshot.configurationComparison.differences.empty();
        ImGui::TextColored(
            identical ? theme.statusGood : theme.statusWarning,
            "%s",
            snapshot.configurationComparison.message.c_str());
    }
    else if (!snapshot.configurationComparison.message.empty())
    {
        ImGui::TextColored(
            theme.statusWarning,
            "%s",
            snapshot.configurationComparison.message.c_str());
    }

    const bool hasProfile = snapshot.profileLoaded;
    const bool hasCapture =
        capture.state == ScpConfigurationState::Complete &&
        capture.quads.size() == Fw2051ScpQuadCount;
    if (!hasProfile && !hasCapture)
    {
        return;
    }

    const Fw2051ScpConfigurationSnapshot* profile = hasProfile
        ? &snapshot.loadedProfile.configuration
        : nullptr;
    const auto flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable(
            "configuration_comparison", 5, flags, ImVec2(0.0F, 430.0F)))
    {
        return;
    }

    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn(
        "Scope", ImGuiTableColumnFlags_WidthFixed, 78.0F);
    ImGui::TableSetupColumn(
        "Register", ImGuiTableColumnFlags_WidthFixed, 82.0F);
    ImGui::TableSetupColumn("Setting");
    ImGui::TableSetupColumn(
        "Profile", ImGuiTableColumnFlags_WidthFixed, 110.0F);
    ImGui::TableSetupColumn(
        "Captured", ImGuiTableColumnFlags_WidthFixed, 110.0F);
    ImGui::TableHeadersRow();

    DrawComparisonRow(
        "Global",
        std::nullopt,
        "VME base",
        profile ? ValueIf(true, profile->baseAddress) : std::nullopt,
        ValueIf(hasCapture, capture.baseAddress),
        true,
        theme);
    DrawComparisonRow(
        "Global", 0x6008U, "Hardware ID",
        profile ? ValueIf(true, profile->hardwareId) : std::nullopt,
        ValueIf(hasCapture, capture.hardwareId), true, theme);
    DrawComparisonRow(
        "Global", 0x600EU, "Firmware revision",
        profile ? ValueIf(true, profile->firmwareRevision) : std::nullopt,
        ValueIf(hasCapture, capture.firmwareRevision), true, theme);
    DrawComparisonRow(
        "Global", 0x6010U, "IRQ level",
        profile ? ValueIf(true, profile->irqLevel) : std::nullopt,
        ValueIf(hasCapture, capture.irqLevel), false, theme);
    DrawComparisonRow(
        "Global", 0x6044U, "Output format",
        profile ? ValueIf(true, profile->outputFormat) : std::nullopt,
        ValueIf(hasCapture, capture.outputFormat), true, theme);

    for (std::size_t quad = 0U; quad < Fw2051ScpQuadCount; ++quad)
    {
        const auto* profileQuad = profile != nullptr &&
                profile->quads.size() == Fw2051ScpQuadCount
            ? &profile->quads[quad]
            : nullptr;
        const auto* capturedQuad = hasCapture
            ? &capture.quads[quad]
            : nullptr;
        const std::string scope = "Quad " + std::to_string(quad);
        for (const auto& definition : Fw2051ScpSettingRegistry)
        {
            const auto profileValue = profileQuad
                ? Fw2051ScpQuadRegisterValue(
                    *profileQuad, definition.registerOffset)
                : std::nullopt;
            const auto capturedValue = capturedQuad
                ? Fw2051ScpQuadRegisterValue(
                    *capturedQuad, definition.registerOffset)
                : std::nullopt;
            DrawComparisonRow(
                scope.c_str(),
                definition.registerOffset,
                definition.name,
                profileValue
                    ? std::optional<std::uint32_t>{*profileValue}
                    : std::nullopt,
                capturedValue
                    ? std::optional<std::uint32_t>{*capturedValue}
                    : std::nullopt,
                definition.registerOffset == 0x614AU,
                theme);
        }
    }
    ImGui::EndTable();
}

} // namespace fidget
