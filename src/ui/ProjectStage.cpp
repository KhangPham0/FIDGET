#include "ui/ProjectStage.h"

#include "imgui.h"
#include "imgui_stdlib.h"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace fidget {
namespace {

ImVec4 StatusColor(TunerStatusLevel level, const Theme& theme)
{
    switch (level)
    {
    case TunerStatusLevel::Information:
        return theme.textDisabled;
    case TunerStatusLevel::Success:
        return theme.statusGood;
    case TunerStatusLevel::Warning:
        return theme.statusWarning;
    case TunerStatusLevel::Error:
        return theme.statusError;
    }
    return theme.textDisabled;
}

} // namespace

ProjectStage::ProjectStage()
{
    m_draft.mvlcHost = "mvlc";
    m_draft.mvlcCommandPort = 32768U;
    m_draft.streamHost = "127.0.0.1";
    m_draft.streamPort = 42333U;
    m_draft.modules.push_back({
        "MDPP-32 SCP",
        0x11000000U,
        MdppBackend::Scp,
        "mdpp1_scp_profile.mwwscp",
    });
}

void ProjectStage::Draw(
    ITunerControl& tunerControl,
    const TunerSnapshot& snapshot,
    const Theme& theme)
{
    if (snapshot.projectActive)
    {
        ImGui::TextColored(
            theme.statusGood,
            "Active: %s / %s",
            snapshot.projectPath.empty()
                ? "unsaved crate project"
                : snapshot.projectPath.c_str(),
            snapshot.activeModuleName.c_str());
        ImGui::Separator();
    }

    DrawEndpoints();
    ImGui::Spacing();
    DrawActiveModuleSelector();
    DrawModuleTable(theme);
    DrawOperations(tunerControl);

    ImGui::Spacing();
    ImGui::TextWrapped("Project file: %s", m_projectPath.c_str());
    ImGui::TextColored(
        StatusColor(m_statusLevel, theme), "%s", m_status.c_str());
}

void ProjectStage::DrawEndpoints()
{
    ImGui::TextUnformatted("Endpoints");
    if (ImGui::BeginTable(
            "project_endpoints",
            2,
            ImGuiTableFlags_SizingStretchProp
                | ImGuiTableFlags_BordersInnerV))
    {
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputText("MVLC host", &m_draft.mvlcHost);
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputScalar(
            "Command port",
            ImGuiDataType_U16,
            &m_draft.mvlcCommandPort);

        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputText("Stream host", &m_draft.streamHost);
        ImGui::SetNextItemWidth(-1.0F);
        ImGui::InputScalar(
            "Stream port", ImGuiDataType_U16, &m_draft.streamPort);
        ImGui::EndTable();
    }
}

void ProjectStage::DrawActiveModuleSelector()
{
    if (m_draft.modules.empty())
    {
        m_activeModuleIndex = 0U;
        ImGui::TextDisabled("Add a module to select an active target.");
        return;
    }

    m_activeModuleIndex = std::min(
        m_activeModuleIndex, m_draft.modules.size() - 1U);
    ImGui::SetNextItemWidth(300.0F);
    if (ImGui::BeginCombo(
            "Active module",
            m_draft.modules[m_activeModuleIndex].name.c_str()))
    {
        for (std::size_t index = 0U;
             index < m_draft.modules.size();
             ++index)
        {
            const bool selected = index == m_activeModuleIndex;
            ImGui::PushID(static_cast<int>(index));
            if (ImGui::Selectable(
                    m_draft.modules[index].name.c_str(), selected))
            {
                m_activeModuleIndex = index;
            }
            if (selected)
            {
                ImGui::SetItemDefaultFocus();
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
}

void ProjectStage::DrawModuleTable(const Theme& theme)
{
    ImGui::TextUnformatted("Modules");
    const auto flags = ImGuiTableFlags_Borders
        | ImGuiTableFlags_RowBg
        | ImGuiTableFlags_Resizable
        | ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("project_modules", 6, flags))
    {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("A32 base", ImGuiTableColumnFlags_WidthFixed, 115.0F);
        ImGui::TableSetupColumn("Backend", ImGuiTableColumnFlags_WidthFixed, 90.0F);
        ImGui::TableSetupColumn("Profile", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Support", ImGuiTableColumnFlags_WidthFixed, 155.0F);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 70.0F);
        ImGui::TableHeadersRow();

        std::size_t removeIndex = m_draft.modules.size();
        for (std::size_t index = 0U;
             index < m_draft.modules.size();
             ++index)
        {
            auto& module = m_draft.modules[index];
            ImGui::PushID(static_cast<int>(index));
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0F);
            ImGui::InputText("##name", &module.name);

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0F);
            ImGui::InputScalar(
                "##base",
                ImGuiDataType_U32,
                &module.baseAddress,
                nullptr,
                nullptr,
                "%08X",
                ImGuiInputTextFlags_CharsHexadecimal);

            ImGui::TableNextColumn();
            int backend = module.backend == MdppBackend::Scp ? 0 : 1;
            ImGui::SetNextItemWidth(-1.0F);
            if (ImGui::Combo("##backend", &backend, "SCP\0QDC\0"))
            {
                module.backend = backend == 0
                    ? MdppBackend::Scp
                    : MdppBackend::Qdc;
            }

            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1.0F);
            ImGui::InputText("##profile", &module.profilePath);

            ImGui::TableNextColumn();
            if (MdppBackendImplemented(module.backend))
            {
                ImGui::TextColored(theme.statusGood, "implemented");
            }
            else
            {
                ImGui::TextColored(
                    theme.statusWarning, "backend not implemented");
            }

            ImGui::TableNextColumn();
            if (ImGui::SmallButton("Remove"))
            {
                removeIndex = index;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();

        if (removeIndex < m_draft.modules.size())
        {
            m_draft.modules.erase(
                m_draft.modules.begin()
                + static_cast<std::ptrdiff_t>(removeIndex));
            if (!m_draft.modules.empty())
            {
                m_activeModuleIndex = std::min(
                    m_activeModuleIndex, m_draft.modules.size() - 1U);
            }
            else
            {
                m_activeModuleIndex = 0U;
            }
        }
    }

    if (m_draft.modules.size() < CrateProjectMaximumModules
        && ImGui::Button("Add module"))
    {
        const auto number = m_draft.modules.size() + 1U;
        m_draft.modules.push_back({
            "MDPP " + std::to_string(number),
            0x11000000U + static_cast<std::uint32_t>(number - 1U) * 0x10000U,
            MdppBackend::Scp,
            "module" + std::to_string(number) + ".mwwscp",
        });
    }
}

void ProjectStage::DrawOperations(ITunerControl& tunerControl)
{
    ImGui::Spacing();
    ImGui::SetNextItemWidth(420.0F);
    ImGui::InputText("Project path", &m_projectPath);

    if (ImGui::Button("Validate"))
    {
        const auto result = ValidateCrateProject(m_draft);
        SetStatus(
            result.success
                ? TunerStatusLevel::Success
                : TunerStatusLevel::Error,
            result.message);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save"))
    {
        const auto result = SaveCrateProject(m_draft, m_projectPath);
        SetStatus(
            result.success
                ? TunerStatusLevel::Success
                : TunerStatusLevel::Error,
            result.message);
    }
    ImGui::SameLine();
    if (ImGui::Button("Load"))
    {
        const auto result = LoadCrateProject(m_projectPath);
        if (result.success && result.project)
        {
            m_draft = *result.project;
            m_activeModuleIndex = 0U;
        }
        SetStatus(
            result.success
                ? TunerStatusLevel::Success
                : TunerStatusLevel::Error,
            result.message);
    }
    ImGui::SameLine();
    if (ImGui::Button("Use"))
    {
        const auto result = ValidateCrateProject(m_draft);
        if (!result.success)
        {
            SetStatus(TunerStatusLevel::Error, result.message);
        }
        else if (m_activeModuleIndex >= m_draft.modules.size())
        {
            SetStatus(
                TunerStatusLevel::Error,
                "Select an active module before using the crate project.");
        }
        else
        {
            UseCrateProjectCommand command;
            command.projectPath = m_projectPath;
            command.project = m_draft;
            command.activeModuleIndex = m_activeModuleIndex;
            tunerControl.Submit(std::move(command));
            SetStatus(
                TunerStatusLevel::Success,
                "The validated crate project was sent to the tuner service.");
        }
    }
}

void ProjectStage::SetStatus(
    TunerStatusLevel level,
    std::string message)
{
    m_statusLevel = level;
    m_status = std::move(message);
}

} // namespace fidget
