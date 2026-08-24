#ifndef FIDGET_CORE_MVME_INIT_SCRIPT_H
#define FIDGET_CORE_MVME_INIT_SCRIPT_H

#include "core/MvmeWorkspace.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fidget {

// Values 0 through 7 select one four-channel quad; 8 broadcasts a following
// banked write to all quads.
inline constexpr std::uint16_t Fw2051ScpBroadcastSelectorValue = 8U;

enum class MvmeInitScriptEvaluationState
{
    Complete,
    CompleteWithUnresolvedNonFrontend,
    Failed,
};

enum class MvmeInitScriptUnresolvedImpact
{
    NonFrontend,
    Frontend,
};

enum class MvmeInitScriptUnresolvedReason
{
    UnsupportedStatement,
    UnsupportedExpression,
    UndefinedVariable,
    InvalidValue,
    InvalidSelector,
    SelectorUnresolved,
    MalformedScript,
};

enum class MvmeInitScriptSelectorScope
{
    Quad,
    Broadcast,
};

struct MvmeInitScriptLocation
{
    std::size_t scriptIndex = 0U;
    std::size_t lineNumber = 0U;
};

struct MvmeInitScriptSelectorAssignment
{
    MvmeInitScriptLocation location;
    std::uint16_t value = 0U;
    MvmeInitScriptSelectorScope scope = MvmeInitScriptSelectorScope::Quad;
};

// One safely resolved write in original script order. A broadcast write is
// represented once with selectorValue 8; finalFrontendValues expands it over
// all eight quads and applies later per-quad overrides.
struct MvmeInitScriptFrontendWrite
{
    MvmeInitScriptLocation location;
    std::uint16_t registerOffset = 0U;
    std::uint16_t value = 0U;
    std::uint16_t selectorValue = 0U;
    MvmeInitScriptSelectorScope selectorScope =
        MvmeInitScriptSelectorScope::Quad;
};

struct MvmeInitScriptFrontendValue
{
    MvmeInitScriptLocation location;
    std::uint16_t quad = 0U;
    std::uint16_t registerOffset = 0U;
    std::uint16_t value = 0U;
};

struct MvmeInitScriptUnresolvedStatement
{
    MvmeInitScriptLocation location;
    MvmeInitScriptUnresolvedImpact impact =
        MvmeInitScriptUnresolvedImpact::NonFrontend;
    MvmeInitScriptUnresolvedReason reason =
        MvmeInitScriptUnresolvedReason::UnsupportedStatement;
    std::string message;
};

struct MvmeInitScriptEvaluation
{
    MvmeInitScriptEvaluationState state =
        MvmeInitScriptEvaluationState::Failed;
    std::string message;
    std::size_t enabledScriptCount = 0U;
    std::vector<MvmeInitScriptSelectorAssignment> selectorAssignments;
    std::vector<MvmeInitScriptFrontendWrite> frontendWrites;
    std::vector<MvmeInitScriptFrontendValue> finalFrontendValues;
    std::vector<MvmeInitScriptUnresolvedStatement> unresolvedStatements;
};

// Passively evaluates the enabled initScripts of an already-located target in
// workspace order. This function never executes a script, contacts hardware,
// or treats workspace values as live restoration evidence. Unsupported or
// unresolvable frontend-affecting statements make the result Failed. Other
// unsupported statements remain explicitly listed for later reporting. The
// resolved write/value collections must not be used when state is Failed.
[[nodiscard]] MvmeInitScriptEvaluation EvaluateMvmeTargetInitScripts(
    const MvmeWorkspace& workspace,
    const MvmeWorkspaceTarget& target);

} // namespace fidget

#endif
