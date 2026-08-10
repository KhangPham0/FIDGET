#ifndef FIDGET_UI_PROJECT_STAGE_H
#define FIDGET_UI_PROJECT_STAGE_H

#include "core/CrateProject.h"
#include "core/TunerControl.h"
#include "ui/Theme.h"

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
        const Theme& theme);

private:
    void DrawEndpoints();
    void DrawActiveModuleSelector();
    void DrawModuleTable(const Theme& theme);
    void DrawOperations(ITunerControl& tunerControl);
    void SetStatus(TunerStatusLevel level, std::string message);

    CrateProject m_draft;
    std::string m_projectPath = "crate.mwwcrate";
    std::size_t m_activeModuleIndex = 0U;
    TunerStatusLevel m_statusLevel = TunerStatusLevel::Information;
    std::string m_status = "Edit or load a crate project, then validate it.";
};

} // namespace fidget

#endif
