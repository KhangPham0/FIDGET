#ifndef FIDGET_UI_STARTUP_STAGE_H
#define FIDGET_UI_STARTUP_STAGE_H

#include "core/TunerControl.h"
#include "ui/Theme.h"

namespace fidget {

class StartupStage
{
public:
    void Draw(
        ITunerControl& tunerControl,
        const TunerSnapshot& snapshot,
        const Theme& theme);

private:
    bool m_confirmed = false;
};

} // namespace fidget

#endif
