#include "ui/StartupAuditStage.h"

#include "core/StartupAudit.h"

#include "imgui.h"

#include <algorithm>
#include <cstdint>
#include <vector>

namespace fidget {
namespace {

ImVec4 AssessmentColor(
    const StartupAuditAssessment assessment,
    const Theme& theme)
{
    switch (assessment)
    {
    case StartupAuditAssessment::Ready:
        return theme.statusGood;
    case StartupAuditAssessment::Blocked:
        return theme.statusError;
    case StartupAuditAssessment::Warning:
        return theme.statusWarning;
    case StartupAuditAssessment::Inherited:
        return theme.accent;
    case StartupAuditAssessment::NotApplicable:
        return theme.textDisabled;
    }
    return theme.textDisabled;
}

ImVec4 ResultColor(const StartupAuditResult& audit, const Theme& theme)
{
    switch (audit.state)
    {
    case StartupAuditState::NotRun:
        return theme.textDisabled;
    case StartupAuditState::Reading:
        return theme.accent;
    case StartupAuditState::Complete:
        return audit.readyForDiagnosticStart
            ? theme.statusGood
            : theme.statusError;
    case StartupAuditState::Failed:
        return theme.statusError;
    }
    return theme.textDisabled;
}

std::vector<const StartupAuditRow*> BlockersFirst(
    const std::vector<StartupAuditRow>& rows)
{
    std::vector<const StartupAuditRow*> ordered;
    ordered.reserve(rows.size());
    for (const auto& row : rows)
    {
        ordered.push_back(&row);
    }
    std::stable_sort(
        ordered.begin(), ordered.end(),
        [](const StartupAuditRow* left, const StartupAuditRow* right) {
            const bool leftBlocked =
                left->assessment == StartupAuditAssessment::Blocked;
            const bool rightBlocked =
                right->assessment == StartupAuditAssessment::Blocked;
            return leftBlocked && !rightBlocked;
        });
    return ordered;
}

} // namespace

void StartupAuditStage::Draw(
    ITunerControl& tunerControl,
    const TunerSnapshot& snapshot,
    const Theme& theme)
{
    ImGui::TextUnformatted("Read-only module-wide startup audit");
    ImGui::TextWrapped(
        "Reads 37 documented module-wide registers for the selected "
        "MDPP-32. This is a true read-only audit: it performs no VME-bus "
        "write. MVLC-local stack writes are used only to execute each VME "
        "read. Required settings are classified without inventing a "
        "replacement value for inherited or experiment-dependent state.");
    ImGui::Spacing();
    ImGui::Text(
        "Target: %s at 0x%08X",
        snapshot.activeModuleName.empty()
            ? "not selected"
            : snapshot.activeModuleName.c_str(),
        static_cast<unsigned>(snapshot.activeModuleBaseAddress));

    const bool sessionOpen =
        snapshot.ownership == GuidedTunerOwnershipState::SessionOpen;
    const bool auditRunning =
        snapshot.activeOperation == GuidedTunerOperation::Audit;
    const bool mayRun =
        sessionOpen && snapshot.targetSupported && !auditRunning;
    ImGui::BeginDisabled(!mayRun);
    if (ImGui::Button("Run 37-register startup audit"))
    {
        tunerControl.Submit(RunStartupAuditCommand{});
    }
    ImGui::EndDisabled();

    if (sessionOpen
        || snapshot.ownership == GuidedTunerOwnershipState::OwnershipLost)
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
            "Open the ownership session before running this audit.");
    }

    const auto& audit = snapshot.startupAudit;
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored(
        ResultColor(audit, theme), "%s", audit.message.c_str());

    if (audit.state != StartupAuditState::Complete)
    {
        return;
    }

    ImGui::Text(
        "Required checks: %zu/%zu | Blocking: %zu | Warnings: %zu",
        audit.requiredReady,
        audit.requiredChecks,
        audit.blockingIssues,
        audit.warnings);
    ImGui::Text(
        "Base 0x%08X | Hardware 0x%04X | Firmware 0x%04X",
        static_cast<unsigned>(audit.baseAddress),
        static_cast<unsigned>(audit.hardwareId),
        static_cast<unsigned>(audit.firmwareRevision));
    ImGui::TextColored(
        audit.readyForDiagnosticStart
            ? theme.statusGood
            : theme.statusError,
        "%s",
        audit.readyForDiagnosticStart
            ? "The required direct-start checks are ready."
            : "A required direct-start check is blocked. Do not start "
              "acquisition.");

    if (audit.blockingIssues > 0U)
    {
        ImGui::Spacing();
        ImGui::TextColored(theme.statusError, "Blocking issues first");
        for (const auto& row : audit.rows)
        {
            if (row.assessment == StartupAuditAssessment::Blocked)
            {
                ImGui::BulletText(
                    "0x%04X %s: %s",
                    static_cast<unsigned>(row.registerOffset),
                    row.name.c_str(),
                    row.note.c_str());
            }
        }
    }

    const auto orderedRows = BlockersFirst(audit.rows);
    const auto flags = ImGuiTableFlags_Borders
        | ImGuiTableFlags_RowBg
        | ImGuiTableFlags_Resizable
        | ImGuiTableFlags_ScrollY
        | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable(
            "startup_audit_rows", 5, flags, ImVec2(0.0F, 340.0F)))
    {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(
            "Register", ImGuiTableColumnFlags_WidthFixed, 78.0F);
        ImGui::TableSetupColumn(
            "Name", ImGuiTableColumnFlags_WidthFixed, 160.0F);
        ImGui::TableSetupColumn(
            "Value", ImGuiTableColumnFlags_WidthFixed, 105.0F);
        ImGui::TableSetupColumn(
            "Role", ImGuiTableColumnFlags_WidthFixed, 85.0F);
        ImGui::TableSetupColumn("Assessment");
        ImGui::TableHeadersRow();

        for (const auto* row : orderedRows)
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text(
                "0x%04X", static_cast<unsigned>(row->registerOffset));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(row->name.c_str());
            ImGui::TableNextColumn();
            ImGui::Text(
                "0x%04X (%u)",
                static_cast<unsigned>(row->value),
                static_cast<unsigned>(row->value));
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(StartupAuditRoleName(row->role));
            ImGui::TableNextColumn();
            ImGui::TextColored(
                AssessmentColor(row->assessment, theme),
                "%s",
                StartupAuditAssessmentName(row->assessment));
            ImGui::SameLine();
            ImGui::TextWrapped("%s", row->note.c_str());
        }
        ImGui::EndTable();
    }
}

} // namespace fidget
