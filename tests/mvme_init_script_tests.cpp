#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/MvmeInitScript.h"
#include "core/MvmeWorkspace.h"
#include "core/MvmeWorkspaceExport.h"
#include "core/TargetModuleAddress.h"

#include <algorithm>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace {

std::string ReadFixture(const std::string& name)
{
    std::ifstream input(
        std::string(FIDGET_TEST_FIXTURE_DIR) + '/' + name,
        std::ios::binary);
    REQUIRE(input.good());
    std::ostringstream contents;
    contents << input.rdbuf();
    REQUIRE_FALSE(input.bad());
    return contents.str();
}

fidget::MvmeWorkspace ParseWorkspace(
    const nlohmann::ordered_json& document)
{
    const auto parsed = fidget::ParseMvmeWorkspace(document.dump());
    REQUIRE(parsed.success);
    REQUIRE(parsed.workspace.has_value());
    return *parsed.workspace;
}

fidget::MvmeWorkspace LoadWorkspace(const std::string& name)
{
    const auto parsed = fidget::ParseMvmeWorkspace(ReadFixture(name));
    REQUIRE(parsed.success);
    REQUIRE(parsed.workspace.has_value());
    return *parsed.workspace;
}

fidget::TargetModuleAddress TargetAddress()
{
    const auto parsed = fidget::ParseTargetModuleAddress("0x1100");
    REQUIRE(parsed.success);
    REQUIRE(parsed.address.has_value());
    return *parsed.address;
}

fidget::MvmeWorkspaceTarget FindTarget(
    const fidget::MvmeWorkspace& workspace)
{
    const auto found = workspace.FindEnabledMdpp32ScpTarget(
        TargetAddress());
    REQUIRE(found.status == fidget::MvmeWorkspaceTargetStatus::Found);
    REQUIRE(found.target.has_value());
    return *found.target;
}

nlohmann::ordered_json Script(
    const std::string& name,
    const std::string& text,
    const bool enabled = true)
{
    return {
        {"id", "script_fixture_" + name},
        {"name", name},
        {"enabled", enabled},
        {"vme_script", text},
    };
}

nlohmann::ordered_json Variable(
    std::string value,
    nlohmann::ordered_json extra = nlohmann::ordered_json::object())
{
    nlohmann::ordered_json result = {
        {"value", std::move(value)},
        {"definitionLocation", ""},
        {"comment", ""},
    };
    for (auto& item : extra.items())
        result[item.key()] = std::move(item.value());
    return result;
}

nlohmann::ordered_json VariableTable(
    std::string name,
    nlohmann::ordered_json variables)
{
    return {
        {"name", std::move(name)},
        {"variables", std::move(variables)},
    };
}

fidget::MvmeInitScriptEvaluation EvaluateScripts(
    const std::vector<nlohmann::ordered_json>& scripts,
    const std::optional<nlohmann::ordered_json>& moduleVariables =
        std::nullopt,
    const std::optional<nlohmann::ordered_json>& eventVariables =
        std::nullopt,
    const std::optional<nlohmann::ordered_json>& daqVariables =
        std::nullopt)
{
    auto document = LoadWorkspace("mvme_workspace_v4.vme").Json();
    auto& daq = document["DAQConfig"];
    auto& event = daq["events"][0];
    auto& module = event["modules"][0];
    module["initScripts"] = nlohmann::ordered_json::array();
    for (const auto& script : scripts)
        module["initScripts"].push_back(script);
    if (moduleVariables.has_value())
        module["variable_table"] = *moduleVariables;
    if (eventVariables.has_value())
        event["variable_table"] = *eventVariables;
    if (daqVariables.has_value())
        daq["variable_table"] = *daqVariables;

    const auto workspace = ParseWorkspace(document);
    return fidget::EvaluateMvmeTargetInitScripts(
        workspace, FindTarget(workspace));
}

const fidget::MvmeInitScriptFrontendValue* FinalValue(
    const fidget::MvmeInitScriptEvaluation& evaluation,
    const std::uint16_t quad,
    const std::uint16_t registerOffset)
{
    const auto found = std::find_if(
        evaluation.finalFrontendValues.begin(),
        evaluation.finalFrontendValues.end(),
        [quad, registerOffset](
            const fidget::MvmeInitScriptFrontendValue& value) {
            return value.quad == quad
                && value.registerOffset == registerOffset;
        });
    return found == evaluation.finalFrontendValues.end()
        ? nullptr
        : &*found;
}

std::size_t UnresolvedCount(
    const fidget::MvmeInitScriptEvaluation& evaluation,
    const fidget::MvmeInitScriptUnresolvedImpact impact)
{
    return static_cast<std::size_t>(std::count_if(
        evaluation.unresolvedStatements.begin(),
        evaluation.unresolvedStatements.end(),
        [impact](const fidget::MvmeInitScriptUnresolvedStatement& item) {
            return item.impact == impact;
        }));
}

} // namespace

TEST_CASE("safe init evaluator resolves the supported corpus constructs")
{
    using namespace fidget;

    const auto evaluation = EvaluateScripts({
        Script("frontend", ReadFixture("mvme_init_frontend.vmescript")),
        Script("overrides", ReadFixture("mvme_init_quad_overrides.vmescript")),
        Script("disabled", "unknown_frontend_command", false),
    });

    REQUIRE(evaluation.state == MvmeInitScriptEvaluationState::Complete);
    CHECK(evaluation.message.find("All enabled") != std::string::npos);
    CHECK(evaluation.enabledScriptCount == 2U);
    CHECK(evaluation.unresolvedStatements.empty());

    REQUIRE(evaluation.selectorAssignments.size() == 3U);
    CHECK(evaluation.selectorAssignments[0].location.scriptIndex == 0U);
    CHECK(evaluation.selectorAssignments[0].location.lineNumber == 12U);
    CHECK(evaluation.selectorAssignments[0].value == 8U);
    CHECK(evaluation.selectorAssignments[0].scope
          == MvmeInitScriptSelectorScope::Broadcast);
    CHECK(evaluation.selectorAssignments[1].location.scriptIndex == 1U);
    CHECK(evaluation.selectorAssignments[1].value == 4U);
    CHECK(evaluation.selectorAssignments[1].scope
          == MvmeInitScriptSelectorScope::Quad);
    CHECK(evaluation.selectorAssignments[2].value == 5U);

    REQUIRE(evaluation.frontendWrites.size() == 13U);
    CHECK(evaluation.frontendWrites.front().registerOffset == 0x6110U);
    CHECK(evaluation.frontendWrites.front().value == 8U);
    CHECK(evaluation.frontendWrites.front().selectorValue == 8U);
    CHECK(evaluation.frontendWrites.front().selectorScope
          == MvmeInitScriptSelectorScope::Broadcast);

    CHECK(evaluation.finalFrontendValues.size() == 72U);
    for (std::uint16_t quad = 0U; quad < 8U; ++quad)
    {
        REQUIRE(FinalValue(evaluation, quad, 0x6110U) != nullptr);
        CHECK(FinalValue(evaluation, quad, 0x6110U)->value == 8U);
        CHECK(FinalValue(evaluation, quad, 0x611AU)->value == 200U);
        CHECK(FinalValue(evaluation, quad, 0x6124U)->value == 160U);
        CHECK(FinalValue(evaluation, quad, 0x6128U)->value == 16U);
        CHECK(FinalValue(evaluation, quad, 0x6146U)->value == 4U);
        CHECK(FinalValue(evaluation, quad, 0x6148U)->value == 12U);
        CHECK(FinalValue(evaluation, quad, 0x614AU)->value == 67U);
    }

    CHECK(FinalValue(evaluation, 0U, 0x611CU)->value == 2000U);
    CHECK(FinalValue(evaluation, 0U, 0x611EU)->value == 2000U);
    CHECK(FinalValue(evaluation, 4U, 0x611CU)->value == 2200U);
    CHECK(FinalValue(evaluation, 4U, 0x611EU)->value == 2200U);
    CHECK(FinalValue(evaluation, 4U, 0x611CU)->location.scriptIndex == 1U);
    CHECK(FinalValue(evaluation, 5U, 0x611CU)->value == 2500U);
    CHECK(FinalValue(evaluation, 5U, 0x611EU)->value == 2600U);
    CHECK(FinalValue(evaluation, 6U, 0x611CU)->value == 2000U);

    CHECK(std::none_of(
        evaluation.frontendWrites.begin(),
        evaluation.frontendWrites.end(),
        [](const MvmeInitScriptFrontendWrite& write) {
            return write.value == 999U;
        }));
}

TEST_CASE("workspace v3 and v4 init scripts are evaluated in source order")
{
    using namespace fidget;

    const auto v3 = LoadWorkspace("mvme_workspace_v3.vme");
    const auto v3Result = EvaluateMvmeTargetInitScripts(v3, FindTarget(v3));
    CHECK(v3Result.state
          == MvmeInitScriptEvaluationState::
              CompleteWithUnresolvedNonFrontend);
    CHECK(v3Result.enabledScriptCount == 1U);
    REQUIRE(v3Result.selectorAssignments.size() == 1U);
    CHECK(v3Result.selectorAssignments[0].value == 0U);
    CHECK(v3Result.frontendWrites.empty());
    CHECK(UnresolvedCount(
              v3Result, MvmeInitScriptUnresolvedImpact::NonFrontend)
          == 1U);

    const auto v4 = LoadWorkspace("mvme_workspace_v4.vme");
    const auto v4Result = EvaluateMvmeTargetInitScripts(v4, FindTarget(v4));
    CHECK(v4Result.state
          == MvmeInitScriptEvaluationState::
              CompleteWithUnresolvedNonFrontend);
    CHECK(v4Result.enabledScriptCount == 2U);
    REQUIRE(v4Result.selectorAssignments.size() == 1U);
    REQUIRE(v4Result.frontendWrites.size() == 1U);
    CHECK(v4Result.selectorAssignments[0].location.scriptIndex == 0U);
    CHECK(v4Result.frontendWrites[0].location.scriptIndex == 1U);
    CHECK(v4Result.frontendWrites[0].registerOffset == 0x6110U);
    CHECK(v4Result.frontendWrites[0].value == 160U);
    REQUIRE(FinalValue(v4Result, 0U, 0x6110U) != nullptr);
    CHECK(FinalValue(v4Result, 0U, 0x6110U)->value == 160U);
}

// MVME src/vme_script_variables.cc at
// fe90d3acd9d6a69aed7eb03ef63282446e54b592 serializes one named symbol table
// with string-valued variable objects and performs no identifier validation.
TEST_CASE("workspace and script variable tables use MVME scope order")
{
    using namespace fidget;

    auto script = Script(
        "variables",
        "0x6100 ${quad}\n"
        "0x6110 $(${timing-value} + ${adjustment})\n"
        "0x611A $(${script-only} + ${module-only} + ${event-only} + "
        "${daq-only} + ${shared})");
    script["variable_table"] = VariableTable(
        "script_scope",
        {
            {"quad", Variable("3")},
            {"adjustment", Variable("4", {{"futureMetadata", 17}})},
            {"script-only", Variable("1")},
            {"shared", Variable("4")},
        });
    const auto moduleVariables = VariableTable(
        "module_scope",
        {
            {"quad", Variable("6")},
            {"timing-value", Variable("12.5")},
            {"module-only", Variable("2")},
            {"shared", Variable("8")},
        });
    const auto eventVariables = VariableTable(
        "event_scope",
        {
            {"timing-value", Variable("20")},
            {"event-only", Variable("3")},
            {"shared", Variable("16")},
        });
    const auto daqVariables = VariableTable(
        "daq_scope",
        {
            {"timing-value", Variable("40")},
            {"daq-only", Variable("4")},
            {"shared", Variable("32")},
        });

    const auto evaluation = EvaluateScripts(
        {script}, moduleVariables, eventVariables, daqVariables);
    REQUIRE(evaluation.state == MvmeInitScriptEvaluationState::Complete);
    REQUIRE(evaluation.selectorAssignments.size() == 1U);
    CHECK(evaluation.selectorAssignments[0].value == 3U);
    REQUIRE(evaluation.frontendWrites.size() == 2U);
    CHECK(evaluation.frontendWrites[0].value == 17U);
    CHECK(evaluation.frontendWrites[1].value == 14U);
    REQUIRE(FinalValue(evaluation, 3U, 0x6110U) != nullptr);
    CHECK(FinalValue(evaluation, 3U, 0x6110U)->value == 17U);

    auto emptyScript = Script("empty_tables", "0x6100 0\n0x6110 8");
    emptyScript["variable_table"] = VariableTable(
        "empty_script_scope", nlohmann::ordered_json::object());
    const auto emptyEvaluation = EvaluateScripts(
        {emptyScript},
        VariableTable("empty_module_scope", nlohmann::ordered_json::object()),
        VariableTable("empty_event_scope", nlohmann::ordered_json::object()),
        VariableTable("empty_daq_scope", nlohmann::ordered_json::object()));
    CHECK(emptyEvaluation.state == MvmeInitScriptEvaluationState::Complete);
}

// MVME src/vme_script.cc at fe90d3acd9d6a69aed7eb03ef63282446e54b592
// performs one initial expansion/evaluation pass, then reparses and expands
// most commands exactly once more. Arithmetic is evaluated only inside $().
TEST_CASE("MVME arithmetic and variable expansion are strict and bounded")
{
    using namespace fidget;

    SUBCASE("bare arithmetic is not a numeric literal")
    {
        const auto evaluation = EvaluateScripts({Script(
            "bare_arithmetic",
            "0x6100 0\n"
            "0x6110 1+2")});
        CHECK(evaluation.state == MvmeInitScriptEvaluationState::Failed);
        CHECK(std::any_of(
            evaluation.unresolvedStatements.begin(),
            evaluation.unresolvedStatements.end(),
            [](const MvmeInitScriptUnresolvedStatement& item) {
                return item.impact
                        == MvmeInitScriptUnresolvedImpact::Frontend
                    && item.reason
                        == MvmeInitScriptUnresolvedReason::
                            ArithmeticRequiresExpression;
            }));
    }

    SUBCASE("explicit arithmetic remains supported")
    {
        const auto evaluation = EvaluateScripts({Script(
            "explicit_arithmetic",
            "0x6100 0\n"
            "0x6110 $(1+2)\n"
            "0x611A $(5*3)")});
        REQUIRE(evaluation.state == MvmeInitScriptEvaluationState::Complete);
        REQUIRE(evaluation.frontendWrites.size() == 2U);
        CHECK(evaluation.frontendWrites[0U].value == 3U);
        CHECK(evaluation.frontendWrites[1U].value == 15U);
    }

    SUBCASE("fractional expression text follows QString default precision")
    {
        const auto evaluation = EvaluateScripts({Script(
            "fractional_arithmetic",
            "0x6100 0\n"
            "0x6110 $(0.4999996)\n"
            "0x611A $(0.49999)")});
        REQUIRE(evaluation.state == MvmeInitScriptEvaluationState::Complete);
        REQUIRE(evaluation.frontendWrites.size() == 2U);
        CHECK(evaluation.frontendWrites[0U].value == 1U);
        CHECK(evaluation.frontendWrites[1U].value == 0U);
    }

    SUBCASE("one command-specific reparse expands a complete command")
    {
        auto script = Script(
            "command_reparse",
            "0x6100 0\n"
            "${command}");
        script["variable_table"] = VariableTable(
            "command_scope",
            {
                {"command", Variable("write a32 d16 0x6110 ${gain}")},
                {"gain", Variable("24")},
            });
        const auto evaluation = EvaluateScripts({script});
        REQUIRE(evaluation.state == MvmeInitScriptEvaluationState::Complete);
        REQUIRE(evaluation.frontendWrites.size() == 1U);
        CHECK(evaluation.frontendWrites[0U].registerOffset == 0x6110U);
        CHECK(evaluation.frontendWrites[0U].value == 24U);
    }

    SUBCASE("a third expansion is never guessed")
    {
        auto script = Script(
            "bounded_expansion",
            "0x6100 0\n"
            "0x6110 ${outer}");
        script["variable_table"] = VariableTable(
            "bounded_scope",
            {
                {"outer", Variable("${middle}")},
                {"middle", Variable("${inner}")},
                {"inner", Variable("24")},
            });
        const auto evaluation = EvaluateScripts({script});
        CHECK(evaluation.state == MvmeInitScriptEvaluationState::Failed);
        CHECK(std::any_of(
            evaluation.unresolvedStatements.begin(),
            evaluation.unresolvedStatements.end(),
            [](const MvmeInitScriptUnresolvedStatement& item) {
                return item.impact
                        == MvmeInitScriptUnresolvedImpact::Frontend
                    && item.reason
                        == MvmeInitScriptUnresolvedReason::
                            ExpansionLimitReached;
            }));
    }
}

TEST_CASE("workspace variable tables reject shapes MVME does not deserialize")
{
    using namespace fidget;

    const std::vector<nlohmann::ordered_json> malformedTables = {
        42,
        {{"variables", nlohmann::ordered_json::object()}},
        {{"name", "bad"}, {"variables", 42}},
        VariableTable("bad", {{"gain", {{"value", "20"}}}}),
        VariableTable("bad", {{"gain", Variable("20")}, {"", Variable("1")}}),
        VariableTable("bad", {{"gain", {
            {"value", 20},
            {"definitionLocation", ""},
            {"comment", ""},
        }}}),
    };
    for (const auto& table : malformedTables)
    {
        const auto evaluation = EvaluateScripts(
            {Script("bad_table", "0x6100 0\n0x6110 20")}, table);
        CHECK(evaluation.state == MvmeInitScriptEvaluationState::Failed);
        CHECK(evaluation.frontendWrites.empty());
        CHECK(std::any_of(
            evaluation.unresolvedStatements.begin(),
            evaluation.unresolvedStatements.end(),
            [](const MvmeInitScriptUnresolvedStatement& item) {
                return item.impact
                        == MvmeInitScriptUnresolvedImpact::Frontend
                    && item.reason
                        == MvmeInitScriptUnresolvedReason::
                            MalformedVariableTable;
            }));
    }
}

TEST_CASE("unsupported non-frontend statements remain visible without guessing")
{
    using namespace fidget;

    const auto evaluation = EvaluateScripts({Script(
        "non_frontend",
        "read a32 d16 0x600E\n"
        "accu_mask_rotate 0x000000ff 32\n"
        "accu_test_warn gte 0x50 \"firmware check\"\n"
        "wait 1ms\n"
        "print \"fixture message\"\n"
        "0x6100 0\n"
        "0x6110 20")});

    CHECK(evaluation.state
          == MvmeInitScriptEvaluationState::
              CompleteWithUnresolvedNonFrontend);
    CHECK_FALSE(evaluation.message.empty());
    CHECK(UnresolvedCount(
              evaluation, MvmeInitScriptUnresolvedImpact::NonFrontend)
          == 5U);
    CHECK(UnresolvedCount(
              evaluation, MvmeInitScriptUnresolvedImpact::Frontend)
          == 0U);
    REQUIRE(evaluation.frontendWrites.size() == 1U);
    CHECK(evaluation.frontendWrites[0].value == 20U);
    for (const auto& unresolved : evaluation.unresolvedStatements)
        CHECK_FALSE(unresolved.message.empty());
}

// MVME src/vme_script.cc at fe90d3acd9d6a69aed7eb03ef63282446e54b592
// parses every command in a script before executing any of them. In
// particular, parseWait() uses ^(\d+)([[:alpha:]]*)$ and accepts only an
// empty, s, ms, or ns suffix. A malformed non-frontend command therefore
// invalidates the whole script just like a malformed frontend command.
TEST_CASE("malformed known non-frontend commands fail the complete script")
{
    using namespace fidget;

    const std::vector<std::string> malformedLines = {
        "wait bananas",
        "read a32 d16",
        "readabs a33 d16 0x600E",
        "accu_mask_rotate 0x000000ff bananas",
        "accu_test_warn eq bananas \"check\"",
        "print \"unterminated",
    };
    for (const auto& malformed : malformedLines)
    {
        CAPTURE(malformed);
        for (const auto malformedFirst : {false, true})
        {
            CAPTURE(malformedFirst);
            const auto script = malformedFirst
                ? malformed + "\n0x6100 0\n0x6110 20"
                : "0x6100 0\n0x6110 20\n" + malformed;
            const auto evaluation = EvaluateScripts({Script(
                "malformed_non_frontend", script)});
            CHECK(evaluation.state == MvmeInitScriptEvaluationState::Failed);
            CHECK(evaluation.selectorAssignments.empty());
            CHECK(evaluation.frontendWrites.empty());
            CHECK(evaluation.finalFrontendValues.empty());
            CHECK_FALSE(
                ExtractFw2051WorkspaceStartingState(evaluation)
                    .startingState.has_value());
        }
    }

    SUBCASE("malformed after accu_test is still a whole-script parse failure")
    {
        const auto evaluation = EvaluateScripts({Script(
            "malformed_after_accu_test",
            "0x6100 0\n"
            "accu_test eq 1 \"live check\"\n"
            "wait bananas\n"
            "0x6110 20")});
        CHECK(evaluation.state == MvmeInitScriptEvaluationState::Failed);
        CHECK_FALSE(evaluation.conditionalAccuTestLocation.has_value());
        CHECK(evaluation.selectorAssignments.empty());
        CHECK(evaluation.frontendWrites.empty());
        CHECK(evaluation.finalFrontendValues.empty());

        Fw2051WorkspaceStartingState forged;
        forged.sourceEvaluationState = evaluation.state;
        forged.frontendValues.push_back(
            {{0U, 4U}, 0U, 0x6110U, 20U});
        CHECK_FALSE(GenerateFw2051LiteralScript(
                        forged, TargetAddress()).success);
        const auto workspace = LoadWorkspace("mvme_workspace_v4.vme");
        CHECK_FALSE(ExportFw2051MvmeWorkspaceCopy(
                        workspace, FindTarget(workspace), forged).success);
    }
}

TEST_CASE("valid known non-frontend command forms remain reportable")
{
    using namespace fidget;

    const auto evaluation = EvaluateScripts({Script(
        "valid_non_frontend_grammar",
        "read a32 d16 0x600E slow fifo\n"
        "readabs A24 D32 0x1000 late mem\n"
        "accu_mask_rotate 0x000000ff 32\n"
        "accu_test_warn gte 0x50 \"firmware check\"\n"
        "wait 1ms\n"
        "wait 1000ns\n"
        "print\n"
        "print \"fixture message\" detail\n"
        "0x6100 0\n"
        "0x6110 20")});

    CHECK(evaluation.state
          == MvmeInitScriptEvaluationState::
              CompleteWithUnresolvedNonFrontend);
    CHECK(evaluation.frontendWrites.size() == 1U);
    CHECK(evaluation.finalFrontendValues.size() == 1U);
    CHECK(UnresolvedCount(
              evaluation, MvmeInitScriptUnresolvedImpact::NonFrontend)
          == 8U);
}

// MVME src/vme_script.cc at fe90d3acd9d6a69aed7eb03ef63282446e54b592
// parses a complete script before returning any commands for execution. A
// ParseError anywhere therefore means no command from that script executes.
TEST_CASE("a parse failure discards every definite write from that script")
{
    using namespace fidget;

    const std::vector<std::string> failureLines = {
        "accu_test eq ${missing} \"check\"",
        "accu_test_warn eq ${missing} \"check\"",
        "set value ${missing}",
        "accu_test invalid 1 \"check\"",
    };
    for (const auto& failureLine : failureLines)
    {
        CAPTURE(failureLine);
        const auto evaluation = EvaluateScripts({Script(
            "whole_script_failure",
            "0x6100 0\n" + failureLine + "\n0x6110 24")});
        CHECK(evaluation.state == MvmeInitScriptEvaluationState::Failed);
        CHECK(evaluation.selectorAssignments.empty());
        CHECK(evaluation.frontendWrites.empty());
        CHECK(evaluation.finalFrontendValues.empty());
        CHECK_FALSE(
            ExtractFw2051WorkspaceStartingState(evaluation)
                .startingState.has_value());
    }

    SUBCASE("a later parse error also withdraws an earlier write")
    {
        const auto evaluation = EvaluateScripts({Script(
            "late_parse_failure",
            "0x6100 0\n"
            "0x6110 20\n"
            "set value ${missing}")});
        CHECK(evaluation.state == MvmeInitScriptEvaluationState::Failed);
        CHECK(evaluation.selectorAssignments.empty());
        CHECK(evaluation.frontendWrites.empty());
        CHECK(evaluation.finalFrontendValues.empty());
    }

    SUBCASE("a failed later script does not erase an earlier valid script")
    {
        const auto evaluation = EvaluateScripts({
            Script("valid_first", "0x6100 1\n0x6110 20"),
            Script(
                "invalid_second",
                "0x6100 2\n0x6110 24\nset value ${missing}"),
        });
        CHECK(evaluation.state == MvmeInitScriptEvaluationState::Failed);
        REQUIRE(evaluation.selectorAssignments.size() == 1U);
        CHECK(evaluation.selectorAssignments[0].value == 1U);
        REQUIRE(evaluation.frontendWrites.size() == 1U);
        CHECK(evaluation.frontendWrites[0].selectorValue == 1U);
        CHECK(evaluation.frontendWrites[0].value == 20U);
        REQUIRE(evaluation.finalFrontendValues.size() == 1U);
        CHECK(evaluation.finalFrontendValues[0].quad == 1U);
        CHECK(evaluation.finalFrontendValues[0].value == 20U);
    }
}

// MVME src/vme_script_exec.cc at fe90d3acd9d6a69aed7eb03ef63282446e54b592
// makes accu_test abort only under AbortOnError, while accu_test_warn always
// continues. FIDGET has neither the live accumulator nor the run option.
TEST_CASE("accu_test makes every later frontend write conditional")
{
    using namespace fidget;

    const auto evaluation = EvaluateScripts({
        Script(
            "conditional_origin",
            "0x6100 0\n"
            "0x6110 20\n"
            "accu_test eq 1 \"live check\"\n"
            "0x6110 22\n"
            "0x611A 100"),
        Script(
            "conditional_later_script",
            "0x6100 2\n"
            "0x6110 24"),
    });

    CHECK(evaluation.state
          == MvmeInitScriptEvaluationState::ConditionalAfterAccuTest);
    REQUIRE(evaluation.conditionalAccuTestLocation.has_value());
    CHECK(evaluation.conditionalAccuTestLocation->scriptIndex == 0U);
    CHECK(evaluation.conditionalAccuTestLocation->lineNumber == 3U);
    REQUIRE(evaluation.frontendWrites.size() == 1U);
    CHECK(evaluation.frontendWrites[0].registerOffset == 0x6110U);
    CHECK(evaluation.frontendWrites[0].value == 20U);
    CHECK(evaluation.finalFrontendValues.empty());
    CHECK(FinalValue(evaluation, 0U, 0x6110U) == nullptr);
    CHECK(FinalValue(evaluation, 0U, 0x611AU) == nullptr);
    CHECK(FinalValue(evaluation, 2U, 0x6110U) == nullptr);
    CHECK(static_cast<std::size_t>(std::count_if(
              evaluation.unresolvedStatements.begin(),
              evaluation.unresolvedStatements.end(),
              [](const MvmeInitScriptUnresolvedStatement& unresolved) {
                  return unresolved.reason
                      == MvmeInitScriptUnresolvedReason::
                          ConditionalAfterAccuTest;
              }))
          == 4U);
}

TEST_CASE("accu_test_warn reports without making later writes conditional")
{
    using namespace fidget;

    const auto evaluation = EvaluateScripts({Script(
        "warning_only",
        "0x6100 0\n"
        "accu_test_warn eq 1 \"live warning\"\n"
        "0x6110 20")});

    CHECK(evaluation.state
          == MvmeInitScriptEvaluationState::
              CompleteWithUnresolvedNonFrontend);
    CHECK_FALSE(evaluation.conditionalAccuTestLocation.has_value());
    REQUIRE(evaluation.frontendWrites.size() == 1U);
    CHECK(evaluation.frontendWrites[0].registerOffset == 0x6110U);
    CHECK(evaluation.frontendWrites[0].value == 20U);
    REQUIRE(evaluation.unresolvedStatements.size() == 1U);
    CHECK(evaluation.unresolvedStatements[0].impact
          == MvmeInitScriptUnresolvedImpact::NonFrontend);
}

TEST_CASE("every unsupported frontend construct fails visibly")
{
    using namespace fidget;

    struct FailureCase
    {
        const char* name;
        std::string text;
        MvmeInitScriptUnresolvedReason reason;
    };
    const std::vector<FailureCase> cases = {
        {
            "undefined value variable",
            "0x6100 0\n0x6110 ${missing}",
            MvmeInitScriptUnresolvedReason::UndefinedVariable,
        },
        {
            "unsupported arithmetic operator",
            "0x6100 0\n0x6110 $(9 % 2)",
            MvmeInitScriptUnresolvedReason::UnsupportedExpression,
        },
        {
            "D16 overflow",
            "0x6100 0\n0x6110 70000",
            MvmeInitScriptUnresolvedReason::InvalidValue,
        },
        {
            "missing frontend value",
            "0x6100 0\n0x6110",
            MvmeInitScriptUnresolvedReason::UnsupportedExpression,
        },
        {
            "absolute frontend write",
            "writeabs a32 d16 0x11006110 20",
            MvmeInitScriptUnresolvedReason::UnsupportedStatement,
        },
        {
            "wrong frontend width",
            "write a32 d32 0x6110 20",
            MvmeInitScriptUnresolvedReason::UnsupportedStatement,
        },
        {
            "unknown potentially writing command",
            "custom_frontend_write 0x6110 20",
            MvmeInitScriptUnresolvedReason::UnsupportedExpression,
        },
        {
            "unterminated block comment",
            "0x6100 0\n/* unfinished",
            MvmeInitScriptUnresolvedReason::MalformedScript,
        },
    };

    for (const auto& failure : cases)
    {
        CAPTURE(failure.name);
        const auto evaluation = EvaluateScripts(
            {Script("unsupported", failure.text)});
        CHECK(evaluation.state == MvmeInitScriptEvaluationState::Failed);
        CHECK_FALSE(evaluation.message.empty());
        CHECK(UnresolvedCount(
                  evaluation, MvmeInitScriptUnresolvedImpact::Frontend)
              >= 1U);
        CHECK(std::any_of(
            evaluation.unresolvedStatements.begin(),
            evaluation.unresolvedStatements.end(),
            [&failure](const MvmeInitScriptUnresolvedStatement& item) {
                return item.impact
                        == MvmeInitScriptUnresolvedImpact::Frontend
                    && item.reason == failure.reason
                    && !item.message.empty();
            }));
    }
}

TEST_CASE("an unresolved selector poisons later banked writes until replaced")
{
    using namespace fidget;

    const auto evaluation = EvaluateScripts({Script(
        "selector_poison",
        "0x6100 ${missing}\n"
        "0x6110 20\n"
        "0x611A 100\n"
        "0x6100 2\n"
        "0x6110 24\n"
        "0x6100 9\n"
        "0x611A 110")});

    REQUIRE(evaluation.state == MvmeInitScriptEvaluationState::Failed);
    CHECK(evaluation.selectorAssignments.empty());
    CHECK(evaluation.frontendWrites.empty());
    CHECK(evaluation.finalFrontendValues.empty());
    CHECK(FinalValue(evaluation, 2U, 0x6110U) == nullptr);
    CHECK(FinalValue(evaluation, 2U, 0x611AU) == nullptr);
    CHECK(UnresolvedCount(
              evaluation, MvmeInitScriptUnresolvedImpact::Frontend)
          == 5U);
    CHECK(static_cast<std::size_t>(std::count_if(
              evaluation.unresolvedStatements.begin(),
              evaluation.unresolvedStatements.end(),
              [](const MvmeInitScriptUnresolvedStatement& item) {
                  return item.reason
                      == MvmeInitScriptUnresolvedReason::SelectorUnresolved;
              }))
          == 3U);
}

TEST_CASE("an unresolved set shadows outer variables and fails later writes")
{
    using namespace fidget;

    const auto moduleVariables = VariableTable(
        "module_scope", {{"gain", Variable("100")}});
    const auto evaluation = EvaluateScripts(
        {Script(
            "set_shadow",
            "set gain\n"
            "0x6100 0\n"
            "0x611A ${gain}")},
        moduleVariables);

    CHECK(evaluation.state == MvmeInitScriptEvaluationState::Failed);
    CHECK(evaluation.frontendWrites.empty());
    CHECK(UnresolvedCount(
              evaluation, MvmeInitScriptUnresolvedImpact::NonFrontend)
          == 1U);
    CHECK(UnresolvedCount(
              evaluation, MvmeInitScriptUnresolvedImpact::Frontend)
          == 1U);
    CHECK(std::any_of(
        evaluation.unresolvedStatements.begin(),
        evaluation.unresolvedStatements.end(),
        [](const MvmeInitScriptUnresolvedStatement& item) {
            return item.impact == MvmeInitScriptUnresolvedImpact::Frontend
                && item.reason
                    == MvmeInitScriptUnresolvedReason::UndefinedVariable;
        }));
}

TEST_CASE("malformed enabled init script metadata fails closed")
{
    using namespace fidget;

    SUBCASE("missing script text")
    {
        auto script = Script("missing_text", "");
        script.erase("vme_script");
        const auto evaluation = EvaluateScripts({script});
        CHECK(evaluation.state == MvmeInitScriptEvaluationState::Failed);
        REQUIRE(evaluation.unresolvedStatements.size() == 1U);
        CHECK(evaluation.unresolvedStatements[0].reason
              == MvmeInitScriptUnresolvedReason::MalformedScript);
    }

    SUBCASE("non-Boolean enabled field")
    {
        auto script = Script("bad_enabled", "0x6100 0");
        script["enabled"] = "yes";
        const auto evaluation = EvaluateScripts({script});
        CHECK(evaluation.state == MvmeInitScriptEvaluationState::Failed);
        REQUIRE(evaluation.unresolvedStatements.size() == 1U);
        CHECK(evaluation.unresolvedStatements[0].impact
              == MvmeInitScriptUnresolvedImpact::Frontend);
    }
}

TEST_CASE("evaluation rejects a target that is not the located enabled module")
{
    using namespace fidget;

    const auto workspace = LoadWorkspace("mvme_workspace_v4.vme");
    auto target = FindTarget(workspace);
    ++target.moduleIndex;
    const auto evaluation = EvaluateMvmeTargetInitScripts(workspace, target);
    CHECK(evaluation.state == MvmeInitScriptEvaluationState::Failed);
    REQUIRE(evaluation.unresolvedStatements.size() == 1U);
    CHECK(evaluation.unresolvedStatements[0].impact
          == MvmeInitScriptUnresolvedImpact::Frontend);
    CHECK(evaluation.unresolvedStatements[0].reason
          == MvmeInitScriptUnresolvedReason::MalformedScript);
}
