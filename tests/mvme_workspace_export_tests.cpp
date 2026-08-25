#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/MvmeInitScript.h"
#include "core/MvmeWorkspace.h"
#include "core/MvmeWorkspaceExport.h"
#include "core/TargetModuleAddress.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
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

fidget::MvmeWorkspace LoadWorkspace(const std::string& name)
{
    const auto parsed = fidget::ParseMvmeWorkspace(ReadFixture(name));
    REQUIRE(parsed.success);
    REQUIRE(parsed.workspace.has_value());
    return *parsed.workspace;
}

fidget::MvmeWorkspace ParseWorkspace(
    const nlohmann::ordered_json& document)
{
    const auto parsed = fidget::ParseMvmeWorkspace(document.dump());
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
    const std::string& id,
    const std::string& name,
    const std::string& text)
{
    return {
        {"id", id},
        {"name", name},
        {"enabled", true},
        {"vme_script", text},
    };
}

fidget::Fw2051WorkspaceStartingState ExampleStartingState()
{
    using namespace fidget;
    Fw2051WorkspaceStartingState state;
    state.sourceEvaluationState = MvmeInitScriptEvaluationState::Complete;
    state.frontendValues = {
        {{1U, 4U}, 2U, 0x611AU, 200U},
        {{0U, 5U}, 0U, 0x6124U, 160U},
        {{0U, 3U}, 0U, 0x6110U, 8U},
    };
    return state;
}

std::size_t CountText(
    const std::string& text,
    const std::string& sought)
{
    std::size_t count = 0U;
    std::size_t position = 0U;
    while ((position = text.find(sought, position)) != std::string::npos)
    {
        ++count;
        position += sought.size();
    }
    return count;
}

class TemporaryDirectory
{
public:
    TemporaryDirectory()
    {
        const auto unique = std::chrono::steady_clock::now()
                                .time_since_epoch().count();
        for (std::size_t index = 0U; index < 1000U; ++index)
        {
            const auto candidate = std::filesystem::temp_directory_path()
                / ("fidget-workspace-export-tests-"
                   + std::to_string(unique) + '-'
                   + std::to_string(index));
            std::error_code createError;
            if (std::filesystem::create_directory(candidate, createError))
            {
                path_ = std::filesystem::weakly_canonical(candidate);
                break;
            }
            REQUIRE_FALSE(createError);
        }
        REQUIRE_FALSE(path_.empty());
    }

    ~TemporaryDirectory()
    {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    const std::filesystem::path& Get() const
    {
        return path_;
    }

private:
    std::filesystem::path path_;
};

void WriteText(
    const std::filesystem::path& path,
    const std::string& text)
{
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    REQUIRE(output.good());
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.close();
    REQUIRE(output.good());
}

std::string ReadText(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    std::ostringstream contents;
    contents << input.rdbuf();
    REQUIRE_FALSE(input.bad());
    return contents.str();
}

} // namespace

TEST_CASE("workspace starting state rejects failed evaluations")
{
    using namespace fidget;

    MvmeInitScriptEvaluation failed;
    failed.state = MvmeInitScriptEvaluationState::Failed;
    failed.finalFrontendValues.push_back(
        {{0U, 2U}, 0U, 0x6110U, 8U});
    auto extracted = ExtractFw2051WorkspaceStartingState(failed);
    CHECK_FALSE(extracted.startingState.has_value());
    CHECK(extracted.message.find("failed") != std::string::npos);

    Fw2051WorkspaceStartingState failedState;
    failedState.sourceEvaluationState = MvmeInitScriptEvaluationState::Failed;
    failedState.frontendValues = failed.finalFrontendValues;
    const auto generated = GenerateFw2051LiteralScript(
        failedState, TargetAddress());
    CHECK_FALSE(generated.success);
    CHECK(generated.text.empty());
    const auto workspace = LoadWorkspace("mvme_workspace_v4.vme");
    const auto exported = ExportFw2051MvmeWorkspaceCopy(
        workspace, FindTarget(workspace), failedState);
    CHECK_FALSE(exported.success);
    CHECK(exported.text.empty());

    MvmeInitScriptEvaluation conditional;
    conditional.state =
        MvmeInitScriptEvaluationState::ConditionalAfterAccuTest;
    conditional.conditionalAccuTestLocation =
        MvmeInitScriptLocation{0U, 2U};
    conditional.finalFrontendValues.push_back(
        {{0U, 1U}, 0U, 0x6110U, 8U});
    extracted = ExtractFw2051WorkspaceStartingState(conditional);
    CHECK_FALSE(extracted.startingState.has_value());
    CHECK(extracted.message.find("conditional") != std::string::npos);

    MvmeInitScriptEvaluation empty;
    empty.state = MvmeInitScriptEvaluationState::Complete;
    extracted = ExtractFw2051WorkspaceStartingState(empty);
    CHECK_FALSE(extracted.startingState.has_value());
    CHECK(extracted.message.find("no FW2051") != std::string::npos);
}

TEST_CASE("workspace starting state retains unresolved non-frontend evidence")
{
    using namespace fidget;

    MvmeInitScriptEvaluation evaluation;
    evaluation.state = MvmeInitScriptEvaluationState::
        CompleteWithUnresolvedNonFrontend;
    evaluation.finalFrontendValues = {
        {{0U, 3U}, 0U, 0x6110U, 8U},
        {{0U, 4U}, 0U, 0x6124U, 160U},
    };
    evaluation.unresolvedStatements.push_back({
        {0U, 1U},
        MvmeInitScriptUnresolvedImpact::NonFrontend,
        MvmeInitScriptUnresolvedReason::UnsupportedStatement,
        "A generic non-frontend fixture statement was not interpreted.",
    });

    const auto extracted = ExtractFw2051WorkspaceStartingState(evaluation);
    REQUIRE(extracted.startingState.has_value());
    CHECK(extracted.startingState->sourceEvaluationState
          == MvmeInitScriptEvaluationState::
              CompleteWithUnresolvedNonFrontend);
    CHECK(extracted.startingState->frontendValues
              .size() == 2U);
    REQUIRE(extracted.startingState->unresolvedNonFrontend.size() == 1U);
    CHECK(extracted.startingState->unresolvedNonFrontend[0].message
          == evaluation.unresolvedStatements[0].message);
    CHECK(extracted.message.find("restoration authority")
          != std::string::npos);
}

TEST_CASE("supported corpus evaluation becomes a validated partial starting state")
{
    using namespace fidget;

    auto document = LoadWorkspace("mvme_workspace_v4.vme").Json();
    auto& scripts = document["DAQConfig"]["events"][0]
                            ["modules"][0]["initScripts"];
    scripts = nlohmann::ordered_json::array({
        Script(
            "script_supported_frontend",
            "supported_frontend",
            ReadFixture("mvme_init_frontend.vmescript")),
        Script(
            "script_supported_overrides",
            "supported_overrides",
            ReadFixture("mvme_init_quad_overrides.vmescript")),
    });
    const auto workspace = ParseWorkspace(document);
    const auto evaluation = EvaluateMvmeTargetInitScripts(
        workspace, FindTarget(workspace));
    REQUIRE(evaluation.state == MvmeInitScriptEvaluationState::Complete);

    const auto extracted = ExtractFw2051WorkspaceStartingState(evaluation);
    REQUIRE(extracted.startingState.has_value());
    CHECK(extracted.startingState->frontendValues.size() == 72U);
    CHECK(extracted.startingState->unresolvedNonFrontend.empty());
    CHECK(extracted.message.find("Live capture") != std::string::npos);
}

TEST_CASE("literal FW2051 script uses sorted writes and explicit settles")
{
    using namespace fidget;

    const auto generated = GenerateFw2051LiteralScript(
        ExampleStartingState(), TargetAddress());
    REQUIRE(generated.success);
    CHECK(generated.valueCount == 3U);
    CHECK(generated.text ==
        "# ===== BEGIN FIDGET Tuned Frontend Settings =====\n"
        "# FIDGET Tuned Frontend Settings\n"
        "# target_a32_base: 0x11000000\n"
        "# expected_module: MDPP-32 SCP (identity comment, not a register write)\n"
        "# expected_firmware: 0x2051 (identity comment, not a register write)\n"
        "# source: workspace init scripts; intended values, not live hardware state\n"
        "# restoration_authority: fresh live capture only\n"
        "# resolved_value_count: 3\n"
        "# unresolved_non_frontend_count: 0\n"
        "\n"
        "# Quad 0\n"
        "write a32 d16 0x6100 0x0000\n"
        "wait 50000ns\n"
        "write a32 d16 0x6110 0x0008\n"
        "wait 20000ns\n"
        "write a32 d16 0x6124 0x00A0\n"
        "wait 20000ns\n"
        "\n"
        "# Quad 2\n"
        "write a32 d16 0x6100 0x0002\n"
        "wait 50000ns\n"
        "write a32 d16 0x611A 0x00C8\n"
        "wait 20000ns\n"
        "\n"
        "# Park the FW2051 bank selector at quad 0\n"
        "write a32 d16 0x6100 0x0000\n"
        "wait 50000ns\n"
        "# ===== END FIDGET Tuned Frontend Settings =====\n");

    auto document = LoadWorkspace("mvme_workspace_v4.vme").Json();
    auto& scripts = document["DAQConfig"]["events"][0]
                            ["modules"][0]["initScripts"];
    scripts = nlohmann::ordered_json::array({
        Script("script_generated_fixture", "generated", generated.text),
    });
    const auto workspace = ParseWorkspace(document);
    const auto evaluation = EvaluateMvmeTargetInitScripts(
        workspace, FindTarget(workspace));
    CHECK(evaluation.state
          == MvmeInitScriptEvaluationState::
              CompleteWithUnresolvedNonFrontend);
    CHECK(evaluation.finalFrontendValues.size() == 3U);
    REQUIRE(evaluation.unresolvedStatements.size() == 6U);
    for (const auto& unresolved : evaluation.unresolvedStatements)
        CHECK(unresolved.impact == MvmeInitScriptUnresolvedImpact::NonFrontend);
}

TEST_CASE("copied workspace appends one final fenced script and preserves fixtures")
{
    using namespace fidget;

    for (const auto* fixture : {
             "mvme_workspace_v3.vme", "mvme_workspace_v4.vme"})
    {
        CAPTURE(fixture);
        const auto workspace = LoadWorkspace(fixture);
        const auto sourceDocument = workspace.Json();
        const auto target = FindTarget(workspace);
        const auto exported = ExportFw2051MvmeWorkspaceCopy(
            workspace, target, ExampleStartingState());
        REQUIRE(exported.success);
        CHECK_FALSE(exported.replacedExistingFidgetScript);
        CHECK(exported.valueCount == 3U);
        CHECK(workspace.Json().dump() == sourceDocument.dump());

        auto exportedDocument = nlohmann::ordered_json::parse(exported.text);
        auto& scripts = exportedDocument["DAQConfig"]["events"]
                                        [target.eventIndex]["modules"]
                                        [target.moduleIndex]["initScripts"];
        REQUIRE(scripts.is_array());
        REQUIRE_FALSE(scripts.empty());
        const auto appended = scripts.back();
        CHECK(appended.at("name") == FidgetTunedFrontendScriptName);
        CHECK(appended.at("enabled") == true);
        CHECK(CountText(
                  appended.at("vme_script").get<std::string>(),
                  FidgetTunedFrontendFenceBegin)
              == 1U);
        CHECK(CountText(
                  appended.at("vme_script").get<std::string>(),
                  FidgetTunedFrontendFenceEnd)
              == 1U);

        scripts.erase(scripts.end() - 1);
        CHECK(exportedDocument.dump() == sourceDocument.dump());
    }
}

TEST_CASE("copied workspace replaces only its own fence and keeps user scripts")
{
    using namespace fidget;

    auto document = LoadWorkspace("mvme_workspace_v4.vme").Json();
    auto& scripts = document["DAQConfig"]["events"][0]
                            ["modules"][0]["initScripts"];
    const auto similarUserScript = Script(
        "script_user_similar",
        FidgetTunedFrontendScriptName,
        "# User-owned script with a similar name.\n0x6100 0");
    scripts.push_back(similarUserScript);

    auto workspace = ParseWorkspace(document);
    auto firstExport = ExportFw2051MvmeWorkspaceCopy(
        workspace, FindTarget(workspace), ExampleStartingState());
    REQUIRE(firstExport.success);
    auto withFidget = nlohmann::ordered_json::parse(firstExport.text);
    auto& firstScripts = withFidget["DAQConfig"]["events"][0]
                                  ["modules"][0]["initScripts"];
    firstScripts.back()["enabled"] = false;
    firstScripts.back()["fidget_owned_extension"] = 41;
    firstScripts.push_back(Script(
        "script_user_after_fidget",
        "user_after_fidget",
        "0x6100 3\n0x6110 20"));

    workspace = ParseWorkspace(withFidget);
    auto changedState = ExampleStartingState();
    changedState.frontendValues[0].value = 240U;
    const auto secondExport = ExportFw2051MvmeWorkspaceCopy(
        workspace, FindTarget(workspace), changedState);
    REQUIRE(secondExport.success);
    CHECK(secondExport.replacedExistingFidgetScript);

    auto replaced = nlohmann::ordered_json::parse(secondExport.text);
    auto& replacedScripts = replaced["DAQConfig"]["events"][0]
                                    ["modules"][0]["initScripts"];
    REQUIRE(replacedScripts.size() == firstScripts.size());
    CHECK(replacedScripts.at(replacedScripts.size() - 2U).at("id")
          == "script_user_after_fidget");
    CHECK(replacedScripts.back().at("name")
          == FidgetTunedFrontendScriptName);
    CHECK(replacedScripts.back().at("enabled") == true);
    CHECK(replacedScripts.back().at("fidget_owned_extension") == 41);
    CHECK(replacedScripts.back().at("vme_script").get<std::string>()
              .find("write a32 d16 0x611A 0x00F0")
          != std::string::npos);

    const auto similar = std::find_if(
        replacedScripts.begin(), replacedScripts.end(),
        [](const auto& candidate) {
            return candidate.is_object() && candidate.contains("id")
                && candidate.at("id") == "script_user_similar";
        });
    REQUIRE(similar != replacedScripts.end());
    CHECK(*similar == similarUserScript);

    std::size_t ownedFenceCount = 0U;
    for (const auto& candidate : replacedScripts)
    {
        if (candidate.is_object() && candidate.contains("vme_script")
            && candidate.at("vme_script").is_string())
        {
            ownedFenceCount += CountText(
                candidate.at("vme_script").get<std::string>(),
                FidgetTunedFrontendFenceBegin);
        }
    }
    CHECK(ownedFenceCount == 1U);

    auto priorUnrelated = firstScripts;
    priorUnrelated.erase(priorUnrelated.end() - 2);
    auto newUnrelated = replacedScripts;
    newUnrelated.erase(newUnrelated.end() - 1);
    CHECK(newUnrelated == priorUnrelated);
}

TEST_CASE("copied workspace fails closed on malformed or ambiguous fences")
{
    using namespace fidget;

    auto document = LoadWorkspace("mvme_workspace_v4.vme").Json();
    auto& scripts = document["DAQConfig"]["events"][0]
                            ["modules"][0]["initScripts"];
    scripts.push_back(Script(
        "script_bad_fence",
        FidgetTunedFrontendScriptName,
        std::string(FidgetTunedFrontendFenceBegin)
            + "\n# missing end marker\n"));
    auto workspace = ParseWorkspace(document);
    auto exported = ExportFw2051MvmeWorkspaceCopy(
        workspace, FindTarget(workspace), ExampleStartingState());
    CHECK_FALSE(exported.success);
    CHECK(exported.text.empty());
    CHECK(exported.message.find("malformed") != std::string::npos);

    const auto generated = GenerateFw2051LiteralScript(
        ExampleStartingState(), TargetAddress());
    REQUIRE(generated.success);
    document = LoadWorkspace("mvme_workspace_v4.vme").Json();
    auto& ambiguousScripts = document["DAQConfig"]["events"][0]
                                    ["modules"][0]["initScripts"];
    ambiguousScripts.push_back(Script(
        "script_fidget_one", FidgetTunedFrontendScriptName, generated.text));
    ambiguousScripts.push_back(Script(
        "script_fidget_two", FidgetTunedFrontendScriptName, generated.text));
    workspace = ParseWorkspace(document);
    exported = ExportFw2051MvmeWorkspaceCopy(
        workspace, FindTarget(workspace), ExampleStartingState());
    CHECK_FALSE(exported.success);
    CHECK(exported.message.find("More than one") != std::string::npos);
}

TEST_CASE("copied workspace save never overwrites its imported source")
{
    using namespace fidget;

    TemporaryDirectory temporary;
    const auto source = temporary.Get() / "source.vme";
    WriteText(source, "source bytes\n");

    auto saved = SaveMvmeWorkspaceCopy(
        "new bytes\n", source.string(), source.string(), true);
    CHECK_FALSE(saved.success);
    CHECK(saved.sourceOverwriteRefused);
    CHECK(ReadText(source) == "source bytes\n");

    const auto alias = temporary.Get() / "source-alias.vme";
    std::error_code linkError;
    std::filesystem::create_hard_link(source, alias, linkError);
    REQUIRE_FALSE(linkError);
    saved = SaveMvmeWorkspaceCopy(
        "new bytes\n", source.string(), alias.string(), true);
    CHECK_FALSE(saved.success);
    CHECK(saved.sourceOverwriteRefused);
    CHECK(ReadText(source) == "source bytes\n");
}

TEST_CASE("atomic copied workspace failure preserves an existing destination")
{
    using namespace fidget;

    TemporaryDirectory temporary;
    const auto source = temporary.Get() / "source.vme";
    const auto destination = temporary.Get() / "copy.vme";
    WriteText(source, "source bytes\n");
    WriteText(destination, "existing destination bytes\n");

    const std::string replacement = "complete replacement bytes\n";
    const auto failed = SaveMvmeWorkspaceCopy(
        replacement,
        source.string(),
        destination.string(),
        true,
        [](std::ostream& output, const std::string_view text) {
            output.write(
                text.data(),
                static_cast<std::streamsize>(text.size() / 2U));
            return false;
        });
    CHECK_FALSE(failed.success);
    CHECK(ReadText(destination) == "existing destination bytes\n");
    CHECK_FALSE(std::filesystem::exists(destination.string() + ".tmp"));

    const auto refused = SaveMvmeWorkspaceCopy(
        replacement, source.string(), destination.string(), false);
    CHECK_FALSE(refused.success);
    CHECK(refused.outputAlreadyExists);
    CHECK(ReadText(destination) == "existing destination bytes\n");

    const auto saved = SaveMvmeWorkspaceCopy(
        replacement, source.string(), destination.string(), true);
    REQUIRE(saved.success);
    CHECK(ReadText(destination) == replacement);
}
