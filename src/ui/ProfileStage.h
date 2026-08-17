#ifndef FIDGET_UI_PROFILE_STAGE_H
#define FIDGET_UI_PROFILE_STAGE_H

#include "core/TunerControl.h"
#include "ui/Theme.h"

#include <string>

namespace fidget {

class ProfileStage
{
public:
    void Draw(
        ITunerControl& tunerControl,
        const TunerSnapshot& snapshot,
        const Theme& theme);

private:
    void AdoptActiveProfilePath(const TunerSnapshot& snapshot);
    void AdoptLoadedProfilePath(const TunerSnapshot& snapshot);

    std::string m_profilePath = "mdpp1_scp_profile.mwwscp";
    std::string m_activeTargetKey;
    std::string m_exportPath;
    std::string m_exportSourcePath;
    bool m_allowExportOverwrite = false;
};

} // namespace fidget

#endif
