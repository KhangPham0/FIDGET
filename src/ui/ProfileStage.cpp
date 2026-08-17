#include "ui/ProfileStage.h"

#include "core/ScpConfiguration.h"

#include "imgui.h"
#include "imgui_stdlib.h"

#include <string>

namespace fidget {

void ProfileStage::Draw(
    ITunerControl& tunerControl,
    const TunerSnapshot& snapshot,
    const Theme& theme)
{
    AdoptActiveProfilePath(snapshot);
    AdoptLoadedProfilePath(snapshot);

    ImGui::TextUnformatted("FW2051 SCP profile");
    ImGui::TextWrapped(
        "A profile is a checksummed local record of 141 configuration "
        "values. Loading or saving a profile performs no hardware I/O.");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(520.0F);
    ImGui::InputText("Profile path", &m_profilePath);

    const bool mayLoad = snapshot.projectActive && snapshot.targetSupported &&
        !m_profilePath.empty();
    ImGui::BeginDisabled(!mayLoad);
    if (ImGui::Button("Load profile"))
    {
        tunerControl.Submit(LoadProfileCommand{m_profilePath});
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    const bool maySave = snapshot.configurationCompleteForTarget &&
        snapshot.configurationFresh && !m_profilePath.empty();
    ImGui::BeginDisabled(!maySave);
    if (ImGui::Button("Save fresh capture"))
    {
        tunerControl.Submit(SaveProfileCommand{m_profilePath});
    }
    ImGui::EndDisabled();

    if (snapshot.ownership == GuidedTunerOwnershipState::SessionOpen ||
        snapshot.ownership == GuidedTunerOwnershipState::OwnershipLost)
    {
        ImGui::SameLine();
        if (ImGui::Button("Release session"))
        {
            tunerControl.Submit(ReleaseSessionCommand{});
        }
    }

    if (!snapshot.profileLoaded)
    {
        ImGui::Spacing();
        ImGui::TextDisabled(
            "No validated FW2051 SCP profile is loaded.");
        return;
    }

    const auto& profile = snapshot.loadedProfile;
    const auto& configuration = profile.configuration;
    ImGui::Spacing();
    ImGui::SeparatorText("Provenance");
    if (!snapshot.profileLoadedForTarget)
    {
        ImGui::TextColored(
            theme.statusWarning,
            "Loaded profile does not match the active module target.");
    }
    else
    {
        ImGui::TextColored(
            theme.statusGood, "Loaded for the active module target");
    }

    if (ImGui::BeginTable(
            "profile_provenance",
            2,
            ImGuiTableFlags_BordersInnerV |
                ImGuiTableFlags_SizingStretchProp))
    {
        const auto row = [](const char* name, const char* value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextDisabled("%s", name);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(value);
        };
        row("Path", snapshot.loadedProfilePath.c_str());
        row("Schema", "MWW SCP profile v1");
        row("Backend", "MDPP-32 SCP FW2051");

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("VME base");
        ImGui::TableNextColumn();
        ImGui::Text(
            "0x%08X", static_cast<unsigned>(configuration.baseAddress));

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Hardware ID");
        ImGui::TableNextColumn();
        ImGui::Text(
            "0x%04X", static_cast<unsigned>(configuration.hardwareId));

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Firmware");
        ImGui::TableNextColumn();
        ImGui::Text(
            "0x%04X", static_cast<unsigned>(configuration.firmwareRevision));

        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Values");
        ImGui::TableNextColumn();
        ImGui::Text("%zu", Fw2051ScpConfigurationValueCount);
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("MVME export");
    ImGui::TextWrapped(
        "Generate a reviewable MVME settings block beside the profile. "
        "This offline operation does not contact crate hardware or edit "
        "the MVME installation.");
    ImGui::SetNextItemWidth(520.0F);
    ImGui::InputText("Export path", &m_exportPath);
    ImGui::Checkbox(
        "Allow replacing an existing export", &m_allowExportOverwrite);

    const bool mayExport = snapshot.profileLoaded
        && !m_exportPath.empty();
    ImGui::BeginDisabled(!mayExport);
    if (ImGui::Button("Export MVME settings"))
    {
        tunerControl.Submit(ExportMvmeScriptCommand{
            m_exportPath,
            m_allowExportOverwrite,
        });
    }
    ImGui::EndDisabled();

    if (!snapshot.mvmeExportMessage.empty())
    {
        ImGui::TextColored(
            snapshot.mvmeExportSucceeded
                ? theme.statusGood
                : theme.statusWarning,
            "%s",
            snapshot.mvmeExportMessage.c_str());
    }
}

void ProfileStage::AdoptActiveProfilePath(
    const TunerSnapshot& snapshot)
{
    if (!snapshot.projectActive)
    {
        m_activeTargetKey.clear();
        return;
    }

    const std::string targetKey = snapshot.projectPath + '#' +
        std::to_string(snapshot.activeModuleIndex) + ':' +
        snapshot.activeModuleProfilePath;
    if (targetKey == m_activeTargetKey)
    {
        return;
    }

    m_activeTargetKey = targetKey;
    if (!snapshot.activeModuleProfilePath.empty())
    {
        m_profilePath = snapshot.activeModuleProfilePath;
    }
}

void ProfileStage::AdoptLoadedProfilePath(
    const TunerSnapshot& snapshot)
{
    if (!snapshot.profileLoaded || snapshot.loadedProfilePath.empty()
        || snapshot.loadedProfilePath == m_exportSourcePath)
    {
        return;
    }

    m_exportSourcePath = snapshot.loadedProfilePath;
    m_exportPath = snapshot.loadedProfilePath + ".mvme";
    m_allowExportOverwrite = false;
}

} // namespace fidget
