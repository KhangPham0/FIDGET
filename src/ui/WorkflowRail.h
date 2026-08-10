#ifndef FIDGET_UI_WORKFLOW_RAIL_H
#define FIDGET_UI_WORKFLOW_RAIL_H

#include "core/GuidedWorkflow.h"
#include "ui/Theme.h"

namespace fidget {

[[nodiscard]] bool DrawWorkflowRail(
    const GuidedTunerDecision& decision,
    const Theme& theme);

} // namespace fidget

#endif
