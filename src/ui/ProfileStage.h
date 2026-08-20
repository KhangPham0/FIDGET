#ifndef FIDGET_UI_PROFILE_STAGE_H
#define FIDGET_UI_PROFILE_STAGE_H

#include "core/TunerControl.h"
#include "ui/Theme.h"
#include "ui/UiDialogs.h"

#include <chrono>
#include <string>

namespace fidget {

class ProfileStage
{
public:
    void Draw(
        ITunerControl& tunerControl,
        const TunerSnapshot& snapshot,
        const Theme& theme,
        UiDialogs& dialogs);

    void ReportFileOperationResult(
        const TunerSnapshot& snapshot, UiDialogs& dialogs);

private:
    enum class FileOperation
    {
        None,
        LoadProfile,
        SaveProfile,
        ExportMvme,
    };

    void AdoptActiveProfilePath(const TunerSnapshot& snapshot);
    void AdoptLoadedProfilePath(const TunerSnapshot& snapshot);
    void BeginFileOperation(FileOperation operation);

    std::string m_profilePath = "mdpp1_scp_profile.mwwscp";
    std::string m_activeTargetKey;
    std::string m_exportPath;
    std::string m_exportSourcePath;
    bool m_allowExportOverwrite = false;
    FileOperation m_pendingFileOperation = FileOperation::None;
    std::chrono::system_clock::time_point m_fileOperationSubmittedAt;
};

} // namespace fidget

#endif
