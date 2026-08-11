#include "ui/StartupStage.h"

#include "core/DeterministicStartup.h"
#include "core/StartupPreparation.h"

#include "imgui.h"

#include <string>

namespace fidget {
namespace {

ImVec4 ResultColor(
    const DeterministicStartupState state,
    const Theme& theme)
{
    switch (state)
    {
    case DeterministicStartupState::NotRun:
        return theme.textDisabled;
    case DeterministicStartupState::PreparingReadout:
    case DeterministicStartupState::CapturingPreparedConfiguration:
    case DeterministicStartupState::ApplyingBankedProfile:
    case DeterministicStartupState::VerifyingFinalConfiguration:
        return theme.accent;
    case DeterministicStartupState::Passed:
        return theme.statusGood;
    case DeterministicStartupState::Failed:
        return theme.statusError;
    }
    return theme.textDisabled;
}

} // namespace

void StartupStage::Draw(
    ITunerControl& tunerControl,
    const TunerSnapshot& snapshot,
    const Theme& theme)
{
    ImGui::TextUnformatted("Deterministic FW2051 startup");
    ImGui::TextWrapped(
        "This reviewed sequence prepares only the eight-register readout "
        "contract, recaptures all eight SCP banks, replans from that fresh "
        "capture, applies the saved banked values with rollback protection, "
        "and recaptures all 141 profile values. It never starts acquisition.");
    ImGui::Spacing();
    ImGui::Text(
        "Target: %s at 0x%08X",
        snapshot.activeModuleName.empty()
            ? "not selected"
            : snapshot.activeModuleName.c_str(),
        static_cast<unsigned>(snapshot.activeModuleBaseAddress));

    const auto& plan = snapshot.standaloneStartupPlan;
    ImGui::SeparatorText("Reviewed recipe");
    ImGui::TextColored(
        plan.success ? theme.statusGood : theme.statusError,
        "%s",
        plan.message.empty()
            ? "No deterministic startup recipe is available."
            : plan.message.c_str());
    if (plan.success)
    {
        ImGui::Text(
            "Initial comparison: %zu values | %zu total difference(s)",
            plan.valuesCompared,
            plan.configurationDifferences);
        ImGui::Text(
            "Profile-visible preparation: %zu | Banked writes: %zu",
            plan.startupContractDifferences,
            plan.bankedApplication.steps.size());
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Eight-register preparation contract");
    if (!snapshot.startupAuditCompleteForTarget)
    {
        ImGui::TextColored(
            theme.statusWarning,
            "A completed startup audit for this exact target is required.");
    }
    else if (snapshot.startupPreparationMismatches.empty())
    {
        ImGui::TextColored(
            theme.statusGood,
            "All eight preparation values already match. The sequence "
            "will still stop, reset, and verify the module.");
    }
    else if (ImGui::BeginTable(
                 "startup_contract_mismatches",
                 4,
                 ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                     ImGuiTableFlags_SizingStretchProp))
    {
        ImGui::TableSetupColumn("Register");
        ImGui::TableSetupColumn("Setting");
        ImGui::TableSetupColumn("Audited");
        ImGui::TableSetupColumn("Required");
        ImGui::TableHeadersRow();
        for (const auto& mismatch : snapshot.startupPreparationMismatches)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text(
                "0x%04X",
                static_cast<unsigned>(mismatch.registerOffset));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(mismatch.name);
            ImGui::TableNextColumn();
            ImGui::TextColored(
                theme.statusWarning,
                "0x%04X",
                static_cast<unsigned>(mismatch.currentValue));
            ImGui::TableNextColumn();
            ImGui::Text(
                "0x%04X",
                static_cast<unsigned>(mismatch.targetValue));
        }
        ImGui::EndTable();
    }

    std::string disabledReason;
    if (snapshot.activeOperation != GuidedTunerOperation::None)
    {
        disabledReason = "Another controlled operation is running.";
    }
    else if (snapshot.ownership != GuidedTunerOwnershipState::SessionOpen)
    {
        disabledReason = "Open the ownership session first.";
    }
    else if (!snapshot.configurationFresh)
    {
        disabledReason = "Recapture all eight SCP quads first.";
    }
    else if (!snapshot.startupAuditCompleteForTarget)
    {
        disabledReason = "Run the startup audit for this target first.";
    }
    else if (!snapshot.startupPlanAvailable)
    {
        disabledReason = plan.message.empty()
            ? "The deterministic startup recipe is not available."
            : plan.message;
    }

    const bool mayRun = disabledReason.empty();
    if (!mayRun)
    {
        m_confirmed = false;
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(!mayRun);
    const std::string confirmation =
        "I reviewed preparing " +
        std::to_string(snapshot.startupPreparationMismatches.size()) +
        " module-wide mismatch(es) and applying " +
        std::to_string(plan.bankedApplication.steps.size()) +
        " banked value(s)";
    ImGui::Checkbox(confirmation.c_str(), &m_confirmed);
    ImGui::BeginDisabled(!m_confirmed);
    ImGui::PushStyleColor(ImGuiCol_Button, theme.statusWarning);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, theme.highlight);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, theme.statusWarning);
    if (ImGui::Button("Run deterministic startup and leave stopped"))
    {
        m_confirmed = false;
        tunerControl.Submit(RunDeterministicStartupCommand{true});
    }
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    ImGui::EndDisabled();
    if (!disabledReason.empty())
    {
        ImGui::TextDisabled("Startup disabled: %s", disabledReason.c_str());
    }

    if (snapshot.ownership == GuidedTunerOwnershipState::SessionOpen ||
        snapshot.ownership == GuidedTunerOwnershipState::OwnershipLost)
    {
        ImGui::SameLine();
        if (ImGui::Button("Release session"))
        {
            tunerControl.Submit(ReleaseSessionCommand{});
        }
    }

    const auto& result = snapshot.deterministicStartupResult;
    if (result.state == DeterministicStartupState::NotRun)
    {
        return;
    }
    ImGui::Spacing();
    ImGui::SeparatorText("Last deterministic startup");
    ImGui::TextColored(
        ResultColor(result.state, theme), "%s", result.message.c_str());
    ImGui::Text(
        "Preparation writes: %zu/%zu verified | Banked writes: %zu",
        result.preparation.writesVerified,
        result.preparation.changedSettings,
        result.bankedWritesPlanned);
    ImGui::Text(
        "Final proof: %zu/141 values | Module stopped: %s",
        result.finalComparison.valuesCompared,
        result.moduleLeftStopped ? "yes" : "not verified");
}

} // namespace fidget
