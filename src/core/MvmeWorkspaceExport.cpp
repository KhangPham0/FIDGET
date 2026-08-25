#include "core/MvmeWorkspaceExport.h"

#include "core/ScpConfiguration.h"
#include "core/ScpRegistry.h"
#include "core/ScpTransactionPlan.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <ostream>
#include <random>
#include <sstream>
#include <system_error>
#include <tuple>
#include <utility>

namespace fidget {
namespace {

constexpr std::size_t MaximumTemporaryCollisions = 100U;
constexpr std::size_t MaximumScriptIdAttempts = 64U;

struct FrontendKey
{
    std::uint16_t quad = 0U;
    std::uint16_t registerOffset = 0U;

    bool operator<(const FrontendKey& other) const noexcept
    {
        return std::tie(quad, registerOffset)
            < std::tie(other.quad, other.registerOffset);
    }
};

struct PlannedLiteralValue
{
    std::uint16_t quad = 0U;
    std::uint16_t registerOffset = 0U;
    std::uint16_t value = 0U;
    bool stagingBoundary = false;
};

struct LiteralValuePlan
{
    bool success = false;
    std::string message;
    std::vector<PlannedLiteralValue> values;
};

std::string Hexadecimal16(const std::uint16_t value)
{
    char text[8]{};
    std::snprintf(
        text, sizeof(text), "0x%04X", static_cast<unsigned>(value));
    return text;
}

std::string Hexadecimal32(const std::uint32_t value)
{
    char text[16]{};
    std::snprintf(
        text, sizeof(text), "0x%08X", static_cast<unsigned>(value));
    return text;
}

std::string ValidateStartingState(
    const Fw2051WorkspaceStartingState& state)
{
    if (state.sourceEvaluationState == MvmeInitScriptEvaluationState::Failed)
        return "A failed init-script evaluation cannot be exported.";
    if (state.sourceEvaluationState
        == MvmeInitScriptEvaluationState::ConditionalAfterAccuTest)
    {
        return "A conditional init-script evaluation cannot be exported.";
    }
    if (state.frontendValues.empty())
        return "The workspace starting state has no resolved frontend values.";

    if (state.sourceEvaluationState == MvmeInitScriptEvaluationState::Complete
        && !state.unresolvedNonFrontend.empty())
    {
        return "A complete evaluation cannot carry unresolved statements.";
    }
    if (state.sourceEvaluationState
            == MvmeInitScriptEvaluationState::
                CompleteWithUnresolvedNonFrontend
        && state.unresolvedNonFrontend.empty())
    {
        return "The partial evaluation does not carry its unresolved list.";
    }
    if (std::any_of(
            state.unresolvedNonFrontend.begin(),
            state.unresolvedNonFrontend.end(),
            [](const MvmeInitScriptUnresolvedStatement& unresolved) {
                return unresolved.impact
                    != MvmeInitScriptUnresolvedImpact::NonFrontend;
            }))
    {
        return "A workspace starting state cannot carry an unresolved "
               "frontend statement.";
    }

    std::map<FrontendKey, std::uint16_t> values;
    for (const auto& value : state.frontendValues)
    {
        if (value.quad >= Fw2051ScpQuadCount)
            return "A workspace frontend value has an invalid quad.";
        const auto* definition = FindFw2051ScpSetting(value.registerOffset);
        if (definition == nullptr)
            return "A workspace frontend value is outside the FW2051 "
                   "setting registry.";
        const auto valueError = ValidateFw2051ScpProfileValue(
            value.registerOffset, value.value);
        if (!valueError.empty())
            return valueError;
        if (!values.emplace(
                FrontendKey{value.quad, value.registerOffset}, value.value)
                 .second)
        {
            return "The workspace starting state contains a duplicate "
                   "frontend value.";
        }
    }

    for (const auto& item : values)
    {
        const auto* definition = FindFw2051ScpSetting(
            item.first.registerOffset);
        if (definition == nullptr
            || definition->dependencyRule
                == Fw2051ScpDependencyRule::None)
        {
            continue;
        }
        const auto dependency = values.find({
            item.first.quad, definition->dependencyRegister});
        if (dependency != values.end()
            && !Fw2051ScpDependencySatisfied(
                *definition, item.second, dependency->second))
        {
            return "A workspace frontend value does not satisfy its resolved "
                   "FW2051 dependency.";
        }
    }
    return {};
}

LiteralValuePlan PlanLiteralValues(
    const Fw2051WorkspaceStartingState& state)
{
    LiteralValuePlan plan;
    std::map<FrontendKey, const MvmeInitScriptFrontendValue*> values;
    for (const auto& value : state.frontendValues)
        values.emplace(
            FrontendKey{value.quad, value.registerOffset}, &value);

    const auto appendCoupled = [&plan, &values](
        const std::uint16_t quad,
        const std::uint16_t constrainedRegister,
        const std::uint16_t boundaryRegister) {
        const auto constrained = values.find({quad, constrainedRegister});
        const auto boundary = values.find({quad, boundaryRegister});
        if (constrained == values.end() && boundary == values.end())
            return true;
        if (constrained == values.end() || boundary == values.end())
        {
            plan.message =
                "A FW2051 coupled-register group is incomplete.";
            return false;
        }

        const auto* boundaryDefinition = FindFw2051ScpSetting(
            boundaryRegister);
        if (boundaryDefinition == nullptr)
        {
            plan.message =
                "The FW2051 coupled boundary is absent from the registry.";
            return false;
        }

        // With no live starting value available to an exported script, first
        // establish the widest valid boundary. The shared transaction planner
        // then orders the constrained target before the final boundary value.
        plan.values.push_back({
            quad,
            boundaryRegister,
            boundaryDefinition->maximumValue,
            true,
        });
        const auto ordered = PlanFw2051ScpCoupledWriteOrder(
            constrainedRegister,
            boundaryRegister,
            boundaryDefinition->maximumValue,
            constrained->second->value,
            boundary->second->value);
        if (!ordered.success)
        {
            plan.message = ordered.message;
            return false;
        }
        for (const auto registerOffset : ordered.registerOffsets)
        {
            const auto value = values.find({quad, registerOffset});
            if (value == values.end())
            {
                plan.message =
                    "The coupled FW2051 export plan lost a target value.";
                return false;
            }
            plan.values.push_back({
                quad, registerOffset, value->second->value, false});
        }
        return true;
    };

    for (std::uint16_t quad = 0U; quad < Fw2051ScpQuadCount; ++quad)
    {
        for (const auto registerOffset :
             Fw2051ScpIndependentRegisterOrder)
        {
            const auto value = values.find({quad, registerOffset});
            if (value != values.end())
            {
                plan.values.push_back({
                    quad, registerOffset, value->second->value, false});
            }
        }
        if (!appendCoupled(quad, 0x6110U, 0x6124U)
            || !appendCoupled(quad, 0x6146U, 0x6148U))
        {
            plan.values.clear();
            return plan;
        }
    }

    plan.success = true;
    plan.message = "Prepared dependency-safe FW2051 literal write order.";
    return plan;
}

std::size_t OccurrenceCount(
    const std::string_view text,
    const std::string_view marker)
{
    std::size_t result = 0U;
    std::size_t position = 0U;
    while ((position = text.find(marker, position)) != std::string_view::npos)
    {
        ++result;
        position += marker.size();
    }
    return result;
}

std::string_view Trim(const std::string_view text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1U);
}

enum class FenceState
{
    None,
    Complete,
    Malformed,
};

FenceState InspectFidgetFence(const std::string_view text)
{
    const std::string_view begin = FidgetTunedFrontendFenceBegin;
    const std::string_view end = FidgetTunedFrontendFenceEnd;
    const auto beginCount = OccurrenceCount(text, begin);
    const auto endCount = OccurrenceCount(text, end);
    if (beginCount == 0U && endCount == 0U)
        return FenceState::None;
    if (beginCount != 1U || endCount != 1U)
        return FenceState::Malformed;

    const auto trimmed = Trim(text);
    if (trimmed.size() < begin.size() + end.size()
        || trimmed.substr(0U, begin.size()) != begin
        || trimmed.substr(trimmed.size() - end.size()) != end)
    {
        return FenceState::Malformed;
    }
    const auto endPosition = trimmed.find(end, begin.size());
    if (endPosition == std::string_view::npos)
        return FenceState::Malformed;
    return FenceState::Complete;
}

bool IsHexadecimal(const char value) noexcept
{
    return std::isxdigit(static_cast<unsigned char>(value)) != 0;
}

bool IsUuidV4(const std::string_view value) noexcept
{
    if (value.size() != 36U
        || value[8] != '-' || value[13] != '-'
        || value[18] != '-' || value[23] != '-')
    {
        return false;
    }
    for (std::size_t index = 0U; index < value.size(); ++index)
    {
        if (index == 8U || index == 13U || index == 18U || index == 23U)
            continue;
        if (!IsHexadecimal(value[index]))
            return false;
    }
    const auto variant = static_cast<char>(
        std::tolower(static_cast<unsigned char>(value[19])));
    return value[14] == '4'
        && (variant == '8' || variant == '9'
            || variant == 'a' || variant == 'b');
}

bool WorkspaceContainsId(
    const MvmeWorkspace::JsonDocument& value,
    const std::string_view sought)
{
    if (value.is_object())
    {
        const auto id = value.find("id");
        if (id != value.end() && id->is_string()
            && id->get_ref<const std::string&>() == sought)
        {
            return true;
        }
        for (const auto& member : value.items())
        {
            if (WorkspaceContainsId(member.value(), sought))
                return true;
        }
    }
    else if (value.is_array())
    {
        for (const auto& member : value)
        {
            if (WorkspaceContainsId(member, sought))
                return true;
        }
    }
    return false;
}

std::string GenerateUuidV4()
{
    std::array<unsigned char, 16U> bytes{};
    try
    {
        std::random_device entropy;
        for (auto& byte : bytes)
            byte = static_cast<unsigned char>(entropy() & 0xFFU);
    }
    catch (...)
    {
        return {};
    }
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0FU) | 0x40U);
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3FU) | 0x80U);

    char text[37]{};
    std::snprintf(
        text,
        sizeof(text),
        "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
        "%02x%02x%02x%02x%02x%02x",
        static_cast<unsigned>(bytes[0]),
        static_cast<unsigned>(bytes[1]),
        static_cast<unsigned>(bytes[2]),
        static_cast<unsigned>(bytes[3]),
        static_cast<unsigned>(bytes[4]),
        static_cast<unsigned>(bytes[5]),
        static_cast<unsigned>(bytes[6]),
        static_cast<unsigned>(bytes[7]),
        static_cast<unsigned>(bytes[8]),
        static_cast<unsigned>(bytes[9]),
        static_cast<unsigned>(bytes[10]),
        static_cast<unsigned>(bytes[11]),
        static_cast<unsigned>(bytes[12]),
        static_cast<unsigned>(bytes[13]),
        static_cast<unsigned>(bytes[14]),
        static_cast<unsigned>(bytes[15]));
    return text;
}

std::string UniqueFidgetScriptId(
    const MvmeWorkspace::JsonDocument& document,
    const MvmeWorkspaceScriptIdGenerator& generator)
{
    for (std::size_t attempt = 0U;
         attempt < MaximumScriptIdAttempts;
         ++attempt)
    {
        std::string candidate;
        try
        {
            candidate = generator ? generator() : GenerateUuidV4();
        }
        catch (...)
        {
            return {};
        }
        if (!IsUuidV4(candidate))
            return {};
        if (!WorkspaceContainsId(document, candidate))
            return candidate;
    }
    return {};
}

std::filesystem::path ComparablePath(
    const std::filesystem::path& path,
    std::error_code& error)
{
    auto comparable = std::filesystem::weakly_canonical(path, error);
    if (!error)
        return comparable;
    error.clear();
    comparable = std::filesystem::absolute(path, error);
    if (!error)
        comparable = comparable.lexically_normal();
    return comparable;
}

void RemoveTemporaryDirectory(const std::filesystem::path& path)
{
    std::error_code removeError;
    std::filesystem::remove_all(path, removeError);
}

} // namespace

Fw2051WorkspaceStartingStateResult ExtractFw2051WorkspaceStartingState(
    const MvmeInitScriptEvaluation& evaluation)
{
    Fw2051WorkspaceStartingStateResult result;
    if (evaluation.state == MvmeInitScriptEvaluationState::Failed)
    {
        result.message =
            "The failed init-script evaluation has no working starting state.";
        return result;
    }
    if (evaluation.state
        == MvmeInitScriptEvaluationState::ConditionalAfterAccuTest)
    {
        result.message =
            "The conditional init-script evaluation has no definite working "
            "starting state.";
        return result;
    }
    if (evaluation.finalFrontendValues.empty())
    {
        result.message =
            "The init scripts resolved no FW2051 frontend starting values.";
        return result;
    }

    Fw2051WorkspaceStartingState state;
    state.sourceEvaluationState = evaluation.state;
    state.frontendValues = evaluation.finalFrontendValues;
    state.unresolvedNonFrontend = evaluation.unresolvedStatements;
    const auto validation = ValidateStartingState(state);
    if (!validation.empty())
    {
        result.message =
            "The resolved workspace values are not a usable starting state: "
            + validation;
        return result;
    }

    result.message = state.unresolvedNonFrontend.empty()
        ? "Extracted a partial workspace starting state. Live capture remains "
          "the only restoration authority."
        : "Extracted a partial workspace starting state and retained the "
          "unresolved non-frontend statements. Live capture remains the only "
          "restoration authority.";
    result.startingState = std::move(state);
    return result;
}

Fw2051LiteralScriptResult GenerateFw2051LiteralScript(
    const Fw2051WorkspaceStartingState& startingState,
    const TargetModuleAddress targetAddress)
{
    Fw2051LiteralScriptResult result;
    const auto validation = ValidateStartingState(startingState);
    if (!validation.empty())
    {
        result.message =
            "The FW2051 literal script cannot be generated: " + validation;
        return result;
    }

    const auto values = PlanLiteralValues(startingState);
    if (!values.success)
    {
        result.message =
            "The FW2051 literal script cannot be generated: "
            + values.message;
        return result;
    }
    std::ostringstream output;
    output << FidgetTunedFrontendFenceBegin << '\n'
           << "# " << FidgetTunedFrontendScriptName << '\n'
           << "# target_a32_base: "
           << Hexadecimal32(targetAddress.FullA32Value()) << '\n'
           << "# expected_module: MDPP-32 SCP (identity comment, not a "
              "register write)\n"
           << "# expected_firmware: 0x2051 (identity comment, not a register "
              "write)\n"
           << "# source: workspace init scripts; intended values, not live "
              "hardware state\n"
           << "# restoration_authority: fresh live capture only\n"
           << "# resolved_value_count: "
           << startingState.frontendValues.size() << '\n'
           << "# unresolved_non_frontend_count: "
           << startingState.unresolvedNonFrontend.size() << "\n\n";

    std::uint16_t currentQuad = Fw2051ScpQuadCount;
    for (const auto& item : values.values)
    {
        if (item.quad != currentQuad)
        {
            if (currentQuad != Fw2051ScpQuadCount)
                output << '\n';
            currentQuad = item.quad;
            output << "# Quad " << currentQuad << '\n'
                   << "write a32 d16 0x6100 "
                   << Hexadecimal16(currentQuad) << '\n'
                   << "wait 1ms\n";
        }
        if (item.stagingBoundary)
        {
            output << "# Establish the widest safe coupled boundary\n";
        }
        output << "write a32 d16 "
               << Hexadecimal16(item.registerOffset) << ' '
               << Hexadecimal16(item.value) << '\n'
               << "wait 1ms\n";
    }

    output << "\n# Park the FW2051 bank selector at quad 0\n"
           << "write a32 d16 0x6100 0x0000\n"
           << "wait 1ms\n"
           << FidgetTunedFrontendFenceEnd << '\n';
    if (!output)
    {
        result.message = "Could not format the FW2051 literal script.";
        return result;
    }

    result.success = true;
    result.text = output.str();
    result.valueCount = startingState.frontendValues.size();
    result.message = "Generated a literal module-init script containing "
        + std::to_string(result.valueCount)
        + " resolved FW2051 frontend values.";
    return result;
}

MvmeWorkspaceCopyExportResult ExportFw2051MvmeWorkspaceCopy(
    const MvmeWorkspace& workspace,
    const MvmeWorkspaceTarget& target,
    const Fw2051WorkspaceStartingState& startingState,
    const MvmeWorkspaceScriptIdGenerator& scriptIdGenerator)
{
    MvmeWorkspaceCopyExportResult result;
    const auto located = workspace.FindEnabledMdpp32ScpTarget(target.address);
    if (located.status != MvmeWorkspaceTargetStatus::Found
        || !located.target.has_value()
        || located.target->eventIndex != target.eventIndex
        || located.target->moduleIndex != target.moduleIndex)
    {
        result.message =
            "The workspace target is no longer the unique enabled "
            "mdpp32_scp module at its normalized address.";
        return result;
    }

    const auto script = GenerateFw2051LiteralScript(
        startingState, target.address);
    if (!script.success)
    {
        result.message = script.message;
        return result;
    }

    try
    {
        auto document = workspace.Json();
        auto& module = document.at("DAQConfig").at("events")
                           .at(target.eventIndex).at("modules")
                           .at(target.moduleIndex);
        auto scripts = module.find("initScripts");
        if (scripts == module.end())
        {
            module["initScripts"] = MvmeWorkspace::JsonDocument::array();
            scripts = module.find("initScripts");
        }
        if (!scripts->is_array())
        {
            result.message =
                "The target initScripts field is not an array.";
            return result;
        }

        std::optional<std::size_t> ownedIndex;
        for (std::size_t index = 0U; index < scripts->size(); ++index)
        {
            const auto& candidate = (*scripts)[index];
            if (!candidate.is_object())
                continue;
            const auto payload = candidate.find("vme_script");
            if (payload == candidate.end() || !payload->is_string())
                continue;
            const auto fence = InspectFidgetFence(
                payload->get_ref<const std::string&>());
            if (fence == FenceState::Malformed)
            {
                result.message =
                    "An existing FIDGET tuned-settings fence is malformed.";
                return result;
            }
            if (fence == FenceState::Complete)
            {
                if (ownedIndex.has_value())
                {
                    result.message =
                        "More than one FIDGET tuned-settings fence is present.";
                    return result;
                }
                ownedIndex = index;
            }
        }

        MvmeWorkspace::JsonDocument fidgetScript;
        if (ownedIndex.has_value())
        {
            fidgetScript = (*scripts)[*ownedIndex];
            scripts->erase(scripts->begin()
                + static_cast<MvmeWorkspace::JsonDocument::difference_type>(
                    *ownedIndex));
            result.replacedExistingFidgetScript = true;
        }
        else
        {
            const auto id = UniqueFidgetScriptId(
                document, scriptIdGenerator);
            if (id.empty())
            {
                result.message =
                    "Could not allocate a unique UUID-v4 identifier for the "
                    "FIDGET script.";
                return result;
            }
            fidgetScript = MvmeWorkspace::JsonDocument{
                {"id", id},
                {"name", FidgetTunedFrontendScriptName},
                {"enabled", true},
                {"vme_script", script.text},
            };
        }
        if (ownedIndex.has_value())
        {
            const auto id = UniqueFidgetScriptId(
                document, scriptIdGenerator);
            if (id.empty())
            {
                result.message =
                    "Could not allocate a unique UUID-v4 identifier for the "
                    "FIDGET script.";
                return result;
            }
            fidgetScript["id"] = id;
        }
        fidgetScript["name"] = FidgetTunedFrontendScriptName;
        fidgetScript["enabled"] = true;
        fidgetScript["vme_script"] = script.text;
        scripts->push_back(std::move(fidgetScript));

        result.text = document.dump(2) + '\n';
        result.valueCount = script.valueCount;
        result.success = true;
        result.message = result.replacedExistingFidgetScript
            ? "Replaced the prior FIDGET fenced script in a copied workspace."
            : "Appended one FIDGET fenced script to a copied workspace.";
        return result;
    }
    catch (const nlohmann::json::exception& error)
    {
        result.message =
            "The preserved workspace cannot accept the export: "
            + std::string(error.what());
        return result;
    }
}

MvmeWorkspaceCopySaveResult SaveMvmeWorkspaceCopy(
    const std::string_view text,
    const std::string& importedSourcePath,
    const std::string& destinationPath,
    const bool allowOverwrite,
    const MvmeWorkspaceCopyWriter& writer)
{
    MvmeWorkspaceCopySaveResult result;
    if (importedSourcePath.empty())
    {
        result.message = "The imported MVME workspace path is empty.";
        return result;
    }
    if (destinationPath.empty())
    {
        result.message = "The copied MVME workspace path is empty.";
        return result;
    }

    const std::filesystem::path source(importedSourcePath);
    const std::filesystem::path destination(destinationPath);
    std::error_code sourcePathError;
    const auto comparableSource = ComparablePath(source, sourcePathError);
    if (sourcePathError)
    {
        result.message = "Could not resolve the imported workspace path: "
            + sourcePathError.message() + '.';
        return result;
    }
    std::error_code destinationPathError;
    const auto comparableDestination = ComparablePath(
        destination, destinationPathError);
    if (destinationPathError)
    {
        result.message = "Could not resolve the copied workspace path: "
            + destinationPathError.message() + '.';
        return result;
    }
    if (comparableSource == comparableDestination)
    {
        result.sourceOverwriteRefused = true;
        result.message =
            "The copied workspace destination is the imported source file.";
        return result;
    }

    std::error_code sourceExistsError;
    const bool sourceExists = std::filesystem::exists(
        source, sourceExistsError);
    if (sourceExistsError)
    {
        result.message = "Could not inspect the imported workspace path: "
            + sourceExistsError.message() + '.';
        return result;
    }
    std::error_code destinationExistsError;
    result.outputAlreadyExists = std::filesystem::exists(
        destination, destinationExistsError);
    if (destinationExistsError)
    {
        result.message = "Could not inspect the copied workspace path: "
            + destinationExistsError.message() + '.';
        return result;
    }
    if (sourceExists && result.outputAlreadyExists)
    {
        std::error_code equivalentError;
        const bool equivalent = std::filesystem::equivalent(
            source, destination, equivalentError);
        if (equivalentError)
        {
            result.message = "Could not compare the source and destination: "
                + equivalentError.message() + '.';
            return result;
        }
        if (equivalent)
        {
            result.sourceOverwriteRefused = true;
            result.message =
                "The copied workspace destination aliases the imported "
                "source file.";
            return result;
        }
    }
    if (result.outputAlreadyExists)
    {
        std::error_code statusError;
        const auto status = std::filesystem::symlink_status(
            destination, statusError);
        if (statusError
            || status.type() != std::filesystem::file_type::regular)
        {
            result.message =
                "The copied workspace destination is not a plain file.";
            return result;
        }
        if (!allowOverwrite)
        {
            result.message =
                "The copied MVME workspace already exists: "
                + destinationPath;
            return result;
        }
    }

    std::filesystem::path temporaryDirectory;
    for (std::size_t index = 0U;
         index < MaximumTemporaryCollisions; ++index)
    {
        temporaryDirectory = destination;
        temporaryDirectory += index == 0U
            ? ".tmp"
            : ".tmp." + std::to_string(index);
        std::error_code createError;
        if (std::filesystem::create_directory(
                temporaryDirectory, createError))
        {
            break;
        }
        if (createError)
        {
            std::error_code collisionError;
            if (!std::filesystem::exists(
                    temporaryDirectory, collisionError)
                || collisionError)
            {
                result.message =
                    "Could not create an atomic workspace export area: "
                    + createError.message() + '.';
                return result;
            }
        }
        temporaryDirectory.clear();
    }
    if (temporaryDirectory.empty())
    {
        result.message =
            "Could not allocate an atomic workspace export area.";
        return result;
    }

    const auto temporaryFile = temporaryDirectory / "workspace.vme";
    std::ofstream output(
        temporaryFile, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        RemoveTemporaryDirectory(temporaryDirectory);
        result.message = "Could not open the temporary workspace copy.";
        return result;
    }

    bool writeSucceeded = false;
    try
    {
        if (writer)
            writeSucceeded = writer(output, text);
        else
        {
            output.write(text.data(), static_cast<std::streamsize>(text.size()));
            writeSucceeded = static_cast<bool>(output);
        }
    }
    catch (...)
    {
        writeSucceeded = false;
    }
    output.flush();
    output.close();
    if (!writeSucceeded || !output)
    {
        RemoveTemporaryDirectory(temporaryDirectory);
        result.message =
            "Writing the copied MVME workspace failed before installation.";
        return result;
    }

    std::error_code installError;
    if (allowOverwrite)
    {
        std::filesystem::rename(
            temporaryFile, destination, installError);
    }
    else
    {
        // The hard-link creation is the no-replace installation primitive:
        // unlike a prior exists() check followed by rename(), it cannot race
        // into overwriting a destination created after validation.
        std::filesystem::create_hard_link(
            temporaryFile, destination, installError);
        if (!installError)
        {
            std::error_code unlinkError;
            std::filesystem::remove(temporaryFile, unlinkError);
        }
    }
    if (installError)
    {
        std::error_code collisionError;
        result.outputAlreadyExists = std::filesystem::exists(
            destination, collisionError) && !collisionError;
        RemoveTemporaryDirectory(temporaryDirectory);
        result.message =
            "Could not atomically install the copied MVME workspace: "
            + installError.message() + '.';
        return result;
    }
    RemoveTemporaryDirectory(temporaryDirectory);

    result.success = true;
    result.message =
        "Saved the copied MVME workspace to '" + destinationPath + "'.";
    return result;
}

} // namespace fidget
