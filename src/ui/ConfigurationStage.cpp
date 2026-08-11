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
    ITunerControl& tunerControl,
    const char* scope,
    const int quad,
    const std::optional<std::uint16_t>& registerOffset,
    const char* setting,
    const std::optional<std::uint32_t>& profileValue,
    const std::optional<std::uint32_t>& capturedValue,
    const bool hexadecimal,
    const bool rowApplyEnabled,
    const char* rowApplyDisabledReason,
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
    ImGui::TableNextColumn();
    if (!different || quad < 0 || !registerOffset)
    {
        ImGui::TextDisabled("-");
        return;
    }

    ImGui::PushID(quad);
    ImGui::PushID(static_cast<int>(*registerOffset));
    ImGui::BeginDisabled(!rowApplyEnabled);
    ImGui::PushStyleColor(ImGuiCol_Button, theme.statusWarning);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.highlight);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.statusWarning);
    char applyLabel[40]{};
    std::snprintf(
        applyLabel,
        sizeof(applyLabel),
        "Apply Q%d 0x%04X",
        quad,
        static_cast<unsigned>(*registerOffset));
    if (ImGui::SmallButton(applyLabel))
    {
        tunerControl.Submit(ApplyProfileRowCommand{
            *registerOffset,
            static_cast<std::uint16_t>(quad),
        });
    }
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    if (!rowApplyEnabled &&
        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    {
        ImGui::SetTooltip("%s", rowApplyDisabledReason);
    }
    ImGui::PopID();
    ImGui::PopID();
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
    const bool mayCapture = sessionOpen && snapshot.targetSupported &&
        snapshot.activeOperation == GuidedTunerOperation::None;
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
            snapshot.singleRepairResult.state !=
                    ScpSingleRepairState::NotRun ||
                snapshot.bulkApplyResult.state != ScpBulkApplyState::NotRun
                ? "A parameter transaction was attempted. Recapture all "
                  "eight quads before comparing or applying another value."
                : "This capture is incomplete or no longer belongs to the "
                  "current live target. Do not use it as current hardware "
                  "state.");
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

    if (snapshot.singleRepairResult.state != ScpSingleRepairState::NotRun)
    {
        const auto& result = snapshot.singleRepairResult;
        ImGui::SeparatorText("Last single-row transaction");
        ImGui::TextWrapped("%s", result.message.c_str());
        if (ImGui::BeginTable(
                "single_apply_outcomes",
                3,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_SizingStretchSame))
        {
            ImGui::TableSetupColumn("Write");
            ImGui::TableSetupColumn("Rollback");
            ImGui::TableSetupColumn("Retained");
            ImGui::TableHeadersRow();
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(
                result.writeVerified
                    ? theme.statusGood
                    : result.writeAttempted
                        ? theme.statusError
                        : theme.textDisabled,
                "%s",
                result.writeVerified
                    ? "verified"
                    : result.writeAttempted ? "attempted, not verified"
                                            : "not attempted");
            ImGui::TableNextColumn();
            ImGui::TextColored(
                result.rollbackVerified
                    ? theme.statusGood
                    : result.rollbackAttempted
                        ? theme.statusError
                        : theme.textDisabled,
                "%s",
                result.rollbackVerified
                    ? "verified"
                    : result.rollbackAttempted ? "attempted, not verified"
                                               : "not required");
            ImGui::TableNextColumn();
            ImGui::TextColored(
                result.profileValueRetained
                    ? theme.statusGood
                    : theme.statusWarning,
                "%s",
                result.profileValueRetained ? "profile value" : "not profile");
            ImGui::EndTable();
        }
    }

    if (snapshot.bulkApplyResult.state != ScpBulkApplyState::NotRun)
    {
        const auto& result = snapshot.bulkApplyResult;
        ImGui::SeparatorText("Last bulk transaction");
        ImGui::TextWrapped("%s", result.message.c_str());
        if (!result.values.empty() && ImGui::BeginTable(
                "bulk_apply_outcomes",
                5,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_SizingStretchProp))
        {
            ImGui::TableSetupColumn("Scope");
            ImGui::TableSetupColumn("Register");
            ImGui::TableSetupColumn("Setting");
            ImGui::TableSetupColumn("Write");
            ImGui::TableSetupColumn("Rollback / retained");
            ImGui::TableHeadersRow();
            for (const auto& value : result.values)
            {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("Quad %d", value.quad);
                ImGui::TableNextColumn();
                ImGui::Text(
                    "0x%04X", static_cast<unsigned>(value.registerOffset));
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(value.settingName.c_str());
                ImGui::TableNextColumn();
                ImGui::TextColored(
                    value.writeVerified
                        ? theme.statusGood
                        : value.writeAttempted
                            ? theme.statusError
                            : theme.textDisabled,
                    "%s",
                    value.writeVerified
                        ? "verified"
                        : value.writeAttempted ? "not verified"
                                               : "not attempted");
                ImGui::TableNextColumn();
                ImGui::TextColored(
                    value.rollbackVerified || value.profileValueRetained
                        ? theme.statusGood
                        : theme.statusWarning,
                    "%s",
                    value.rollbackVerified
                        ? "original restored"
                        : value.profileValueRetained
                            ? "profile retained"
                            : value.rollbackAttempted
                                ? "rollback not verified"
                                : "not retained");
            }
            ImGui::EndTable();
        }
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

    std::size_t bankedDifferenceCount = 0U;
    for (const auto& difference :
         snapshot.configurationComparison.differences)
    {
        if (difference.quad >= 0 && difference.hasRegister &&
            difference.registerOffset <= 0xFFFFU &&
            FindFw2051ScpSetting(
                static_cast<std::uint16_t>(difference.registerOffset)) !=
                nullptr)
        {
            ++bankedDifferenceCount;
        }
    }

    const bool operationRunning = snapshot.activeOperation !=
        GuidedTunerOperation::None;
    std::string bulkDisabledReason;
    if (snapshot.activeOperation != GuidedTunerOperation::None)
    {
        bulkDisabledReason = "Another controlled operation is running.";
    }
    else if (!sessionOpen)
    {
        bulkDisabledReason = "Open the ownership session first.";
    }
    else if (!snapshot.configurationFresh ||
             !snapshot.configurationComparison.comparable)
    {
        bulkDisabledReason =
            "Recapture all eight quads before applying profile values.";
    }
    else if (!snapshot.profileApplicationPlan.success)
    {
        bulkDisabledReason = snapshot.profileApplicationPlan.message;
    }
    else if (bankedDifferenceCount == 0U)
    {
        bulkDisabledReason = "There are no banked differences to apply.";
    }

    const bool mayApplyBulk = bulkDisabledReason.empty();
    if (!mayApplyBulk)
    {
        m_bulkApplyConfirmed = false;
    }
    ImGui::SeparatorText("Parameter application");
    ImGui::TextWrapped(
        "Every write is ownership-gated and readback-verified. A bulk "
        "transaction first recaptures all 140 hardware registers, leaves "
        "the module stopped, and makes this comparison stale.");
    ImGui::BeginDisabled(!mayApplyBulk);
    ImGui::Checkbox(
        ("I confirm applying " + std::to_string(bankedDifferenceCount) +
         " banked value(s) and leaving the module stopped")
            .c_str(),
        &m_bulkApplyConfirmed);
    ImGui::BeginDisabled(!m_bulkApplyConfirmed);
    ImGui::PushStyleColor(ImGuiCol_Button, theme.statusWarning);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.highlight);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.statusWarning);
    const std::string bulkLabel = "Apply all " +
        std::to_string(bankedDifferenceCount) + " banked differences";
    if (ImGui::Button(bulkLabel.c_str()))
    {
        m_bulkApplyConfirmed = false;
        tunerControl.Submit(ApplyAllDifferencesCommand{});
    }
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (!bulkDisabledReason.empty())
    {
        ImGui::TextDisabled("Apply disabled: %s", bulkDisabledReason.c_str());
    }

    const bool rowApplyEnabled = sessionOpen &&
        snapshot.configurationFresh &&
        snapshot.configurationComparison.comparable &&
        snapshot.activeOperation == GuidedTunerOperation::None;
    const char* rowApplyDisabledReason = operationRunning
        ? "Another controlled operation is running."
        : !sessionOpen
            ? "Open the ownership session first."
            : !snapshot.configurationFresh
                ? "Recapture all eight quads first."
                : "The profile and capture are not comparable.";
    const auto flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_SizingStretchProp;
    if (!ImGui::BeginTable(
            "configuration_comparison", 6, flags, ImVec2(0.0F, 430.0F)))
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
    ImGui::TableSetupColumn(
        "Action", ImGuiTableColumnFlags_WidthFixed, 150.0F);
    ImGui::TableHeadersRow();

    DrawComparisonRow(
        tunerControl,
        "Global",
        -1,
        std::nullopt,
        "VME base",
        profile ? ValueIf(true, profile->baseAddress) : std::nullopt,
        ValueIf(hasCapture, capture.baseAddress),
        true,
        false,
        "Global settings are not applied here.",
        theme);
    DrawComparisonRow(
        tunerControl, "Global", -1, 0x6008U, "Hardware ID",
        profile ? ValueIf(true, profile->hardwareId) : std::nullopt,
        ValueIf(hasCapture, capture.hardwareId), true, false,
        "Global settings are not applied here.", theme);
    DrawComparisonRow(
        tunerControl, "Global", -1, 0x600EU, "Firmware revision",
        profile ? ValueIf(true, profile->firmwareRevision) : std::nullopt,
        ValueIf(hasCapture, capture.firmwareRevision), true, false,
        "Global settings are not applied here.", theme);
    DrawComparisonRow(
        tunerControl, "Global", -1, 0x6010U, "IRQ level",
        profile ? ValueIf(true, profile->irqLevel) : std::nullopt,
        ValueIf(hasCapture, capture.irqLevel), false, false,
        "Global settings are not applied here.", theme);
    DrawComparisonRow(
        tunerControl, "Global", -1, 0x6044U, "Output format",
        profile ? ValueIf(true, profile->outputFormat) : std::nullopt,
        ValueIf(hasCapture, capture.outputFormat), true, false,
        "Global settings are not applied here.", theme);

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
                tunerControl,
                scope.c_str(),
                static_cast<int>(quad),
                definition.registerOffset,
                definition.name,
                profileValue
                    ? std::optional<std::uint32_t>{*profileValue}
                    : std::nullopt,
                capturedValue
                    ? std::optional<std::uint32_t>{*capturedValue}
                    : std::nullopt,
                definition.registerOffset == 0x614AU,
                rowApplyEnabled,
                rowApplyDisabledReason,
                theme);
        }
    }
    ImGui::EndTable();
}

} // namespace fidget
