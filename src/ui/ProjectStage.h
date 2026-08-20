#ifndef FIDGET_UI_PROJECT_STAGE_H
#define FIDGET_UI_PROJECT_STAGE_H

#include "core/CrateProject.h"
#include "core/TunerControl.h"
#include "ui/Theme.h"
#include "ui/UiDialogs.h"

#include <cstddef>
#include <string>

namespace fidget {

class ProjectStage
{
public:
    ProjectStage();

    void Draw(
        ITunerControl& tunerControl,
        const TunerSnapshot& snapshot,
        const Theme& theme,
        UiDialogs& dialogs);

private:
    void DrawEndpoints();
    void DrawActiveModuleSelector();
    void DrawModuleTable(const Theme& theme, UiDialogs& dialogs);
    void DrawOperations(
        ITunerControl& tunerControl, UiDialogs& dialogs);
    void SetStatus(TunerStatusLevel level, std::string message);

    CrateProject m_draft;
    std::string m_projectPath = "crate.mwwcrate";
    std::size_t m_activeModuleIndex = 0U;
    TunerStatusLevel m_statusLevel = TunerStatusLevel::Information;
    std::string m_status = "Edit or load a crate project, then validate it.";
};

} // namespace fidget

#endif
