#ifndef FIDGET_UI_SESSION_STAGE_H
#define FIDGET_UI_SESSION_STAGE_H

#include "core/TunerControl.h"
#include "ui/Theme.h"

namespace fidget {

class SessionStage
{
public:
    void Draw(
        ITunerControl& tunerControl,
        const TunerSnapshot& snapshot,
        const Theme& theme);

private:
    bool m_recoveryConfirmed = false;
};

} // namespace fidget

#endif
