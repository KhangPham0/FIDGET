#ifndef FIDGET_CORE_MVME_WORKSPACE_EXPORT_H
#define FIDGET_CORE_MVME_WORKSPACE_EXPORT_H

#include "core/MvmeInitScript.h"
#include "core/MvmeWorkspace.h"
#include "core/TargetModuleAddress.h"

#include <cstddef>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fidget {

inline constexpr char FidgetTunedFrontendScriptName[] =
    "FIDGET Tuned Frontend Settings";
inline constexpr char FidgetTunedFrontendFenceBegin[] =
    "# ===== BEGIN FIDGET Tuned Frontend Settings =====";
inline constexpr char FidgetTunedFrontendFenceEnd[] =
    "# ===== END FIDGET Tuned Frontend Settings =====";

// A partial working baseline recovered from intended workspace script values.
// It is never evidence of live hardware state and is never restoration
// authority. Before any use, missing values must come from a fresh live capture
// and the combined configuration must be validated. The fresh live capture is
// the only restoration authority.
struct Fw2051WorkspaceStartingState
{
    MvmeInitScriptEvaluationState sourceEvaluationState =
        MvmeInitScriptEvaluationState::Failed;
    std::vector<MvmeInitScriptFrontendValue> frontendValues;
    std::vector<MvmeInitScriptUnresolvedStatement> unresolvedNonFrontend;
};

struct Fw2051WorkspaceStartingStateResult
{
    std::string message;
    std::optional<Fw2051WorkspaceStartingState> startingState;
};

// Failed and ConditionalAfterAccuTest evaluations never produce a starting
// state. A successful evaluation with no resolved frontend values also has no
// working baseline. Non-frontend unresolved statements remain attached to an
// otherwise usable partial state.
[[nodiscard]] Fw2051WorkspaceStartingStateResult
ExtractFw2051WorkspaceStartingState(
    const MvmeInitScriptEvaluation& evaluation);

struct Fw2051LiteralScriptResult
{
    bool success = false;
    std::string message;
    std::string text;
    std::size_t valueCount = 0U;
};

// Generates a module-relative MVME init script containing literal selector and
// FW2051 frontend writes plus their explicit settle waits. Address and firmware
// lines are comments, not hardware claims or register writes.
[[nodiscard]] Fw2051LiteralScriptResult GenerateFw2051LiteralScript(
    const Fw2051WorkspaceStartingState& startingState,
    TargetModuleAddress targetAddress);

struct MvmeWorkspaceCopyExportResult
{
    bool success = false;
    bool replacedExistingFidgetScript = false;
    std::string message;
    std::string text;
    std::size_t valueCount = 0U;
};

// Copies the preserved ordered workspace document and appends one final,
// enabled FIDGET-owned fenced script to the located target. One prior script
// carrying the exact FIDGET fence is replaced and moved last. Similar names
// without the reserved fence remain user-owned and untouched.
[[nodiscard]] MvmeWorkspaceCopyExportResult ExportFw2051MvmeWorkspaceCopy(
    const MvmeWorkspace& workspace,
    const MvmeWorkspaceTarget& target,
    const Fw2051WorkspaceStartingState& startingState);

using MvmeWorkspaceCopyWriter =
    std::function<bool(std::ostream&, std::string_view)>;

struct MvmeWorkspaceCopySaveResult
{
    bool success = false;
    bool outputAlreadyExists = false;
    bool sourceOverwriteRefused = false;
    std::string message;
};

// Atomically installs a copied workspace from a sibling temporary directory.
// The imported source path is always protected, even when overwrite permission
// is true. The optional writer is a deterministic failure-injection seam.
[[nodiscard]] MvmeWorkspaceCopySaveResult SaveMvmeWorkspaceCopy(
    std::string_view text,
    const std::string& importedSourcePath,
    const std::string& destinationPath,
    bool allowOverwrite,
    const MvmeWorkspaceCopyWriter& writer = {});

} // namespace fidget

#endif
