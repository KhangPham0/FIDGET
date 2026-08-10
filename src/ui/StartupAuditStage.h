#ifndef FIDGET_UI_STARTUP_AUDIT_STAGE_H
#define FIDGET_UI_STARTUP_AUDIT_STAGE_H

#include "core/TunerControl.h"
#include "ui/Theme.h"

namespace fidget {

class StartupAuditStage
{
public:
    void Draw(
        ITunerControl& tunerControl,
        const TunerSnapshot& snapshot,
        const Theme& theme);
};

} // namespace fidget

#endif
