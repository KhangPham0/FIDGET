#ifndef FIDGET_CORE_TUNER_WORKSPACE_H
#define FIDGET_CORE_TUNER_WORKSPACE_H

#include "core/MvmeInitScript.h"
#include "core/MvmeWorkspace.h"
#include "core/TargetModuleAddress.h"

#include <optional>
#include <string>
#include <vector>

namespace fidget {

enum class TunerWorkspaceLoadOutcome
{
    NotSelected,
    Loading,
    Loaded,
    FileUnavailable,
    ParseFailed,
};

// Passive optional workspace state for the project-independent GUI path. The
// parsed ordered document and its script-derived values are intended inputs
// only. They never describe live hardware and are never restoration authority;
// only a later fresh live capture can establish a restore point.
struct TunerWorkspaceState
{
    TunerWorkspaceLoadOutcome outcome =
        TunerWorkspaceLoadOutcome::NotSelected;
    std::string sourcePath;
    std::string message;
    std::optional<MvmeWorkspace> workspace;

    bool targetLookupPerformed = false;
    std::optional<TargetModuleAddress> evaluatedTargetAddress;
    MvmeWorkspaceTargetLookupResult targetLookup;

    bool evaluationPerformed = false;
    MvmeInitScriptEvaluation evaluation;
    std::vector<std::string> warnings;
};

} // namespace fidget

#endif
