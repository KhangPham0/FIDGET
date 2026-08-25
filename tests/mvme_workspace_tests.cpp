#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/MvmeWorkspace.h"
#include "core/TargetModuleAddress.h"

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

fidget::MvmeWorkspace LoadFixture(const std::string& name)
{
    const auto parsed = fidget::ParseMvmeWorkspace(ReadFixture(name));
    REQUIRE(parsed.success);
    REQUIRE(parsed.workspace.has_value());
    return *parsed.workspace;
}

fidget::TargetModuleAddress Address(const std::string& text)
{
    const auto parsed = fidget::ParseTargetModuleAddress(text);
    REQUIRE(parsed.success);
    REQUIRE(parsed.address.has_value());
    return *parsed.address;
}

std::vector<std::string> ObjectKeys(
    const nlohmann::ordered_json& object)
{
    std::vector<std::string> result;
    for (const auto& item : object.items())
        result.push_back(item.key());
    return result;
}

fidget::MvmeWorkspace Reparse(
    const nlohmann::ordered_json& document)
{
    const auto parsed = fidget::ParseMvmeWorkspace(document.dump());
    REQUIRE(parsed.success);
    REQUIRE(parsed.workspace.has_value());
    return *parsed.workspace;
}

} // namespace

TEST_CASE("MVME workspace schemas v3 and v4 preserve their ordered documents")
{
    using namespace fidget;

    struct FixtureCase
    {
        const char* name;
        MvmeWorkspaceSchemaVersion version;
        std::vector<std::string> daqKeys;
    };

    const std::vector<FixtureCase> cases = {
        {
            "mvme_workspace_v3.vme",
            MvmeWorkspaceSchemaVersion::V3,
            {
                "id",
                "name",
                "enabled",
                "properties",
                "vme_controller",
                "events",
                "global_objects",
                "variable_table",
                "fixture_unknown_object",
            },
        },
        {
            "mvme_workspace_v4.vme",
            MvmeWorkspaceSchemaVersion::V4,
            {
                "id",
                "name",
                "enabled",
                "properties",
                "vme_controller",
                "variable_table",
                "events",
                "global_objects",
                "fixture_unknown_object",
            },
        },
    };

    for (const auto& fixture : cases)
    {
        CAPTURE(fixture.name);
        const auto sourceText = ReadFixture(fixture.name);
        const auto sourceDocument = nlohmann::ordered_json::parse(sourceText);
        const auto parsed = ParseMvmeWorkspace(sourceText);
        REQUIRE(parsed.success);
        REQUIRE(parsed.workspace.has_value());
        const auto& workspace = *parsed.workspace;
        CHECK(workspace.SchemaVersion() == fixture.version);
        CHECK(workspace.Json() == sourceDocument);
        CHECK(workspace.Json().dump() == sourceDocument.dump());
        CHECK(ObjectKeys(workspace.Json().at("DAQConfig"))
              == fixture.daqKeys);

        const auto serialized = workspace.Serialize();
        const auto roundTrip = ParseMvmeWorkspace(serialized);
        REQUIRE(roundTrip.success);
        REQUIRE(roundTrip.workspace.has_value());
        CHECK(roundTrip.workspace->Json() == workspace.Json());
        CHECK(roundTrip.workspace->Serialize() == serialized);
        CHECK(ObjectKeys(roundTrip.workspace->Json().at("DAQConfig"))
              == fixture.daqKeys);
    }
}

TEST_CASE("MVME workspace round trips scripts ids settings events and extensions")
{
    const auto v3 = LoadFixture("mvme_workspace_v3.vme");
    const auto v3RoundTrip = Reparse(v3.Json());
    const auto& v3Config = v3RoundTrip.Json().at("DAQConfig");
    CHECK(v3Config.at("id") == "daq_fixture_v3");
    CHECK(v3Config.at("vme_controller").at("settings")
              .at("fixture_controller_setting")
          == 23);
    CHECK(v3Config.at("events").at(1).at("modules").at(0)
              .at("initScripts").at(0).at("vme_script")
          == "write a32 d16 0x6000 1\nwrite a32 d16 0x6100 0");
    CHECK(v3Config.at("events").at(1).at("id")
          == "event_fixture_target");
    CHECK(v3Config.at("global_objects").at("fixture_object").at("values")
          == nlohmann::ordered_json::array({3, 1, 4}));
    CHECK(ObjectKeys(v3Config.at("fixture_unknown_object"))
          == std::vector<std::string>{"first", "second", "nested"});

    const auto v4 = LoadFixture("mvme_workspace_v4.vme");
    const auto v4RoundTrip = Reparse(v4.Json());
    const auto& v4Config = v4RoundTrip.Json().at("DAQConfig");
    CHECK(v4Config.at("id") == "daq_fixture_v4");
    CHECK(v4Config.at("vme_controller").at("settings")
              .at("fixture_controller_setting")
          == 31);
    CHECK(v4Config.at("events").at(0).at("modules").at(0)
              .at("initScripts").at(1).at("id")
          == "script_target_init_v4_b");
    CHECK(v4Config.at("events").at(0).at("modules").at(0)
              .at("initScripts").at(1).at("variable_table")
              .at("variables").at("fixture_script_variable")
              .at("fixtureVariableExtension").at("kept")
          == true);
    CHECK(v4Config.at("events").at(1).at("id")
          == "event_fixture_aux_v4");
    CHECK(v4Config.at("events").at(0).at("modules").at(0)
              .at("fixture_module_extension").at("beta")
          == 2);
    CHECK(ObjectKeys(v4Config.at("fixture_unknown_object"))
          == std::vector<std::string>{
              "ordered_first", "ordered_second", "ordered_third"});
}

TEST_CASE("enabled mdpp32_scp target lookup normalizes one address across events")
{
    using namespace fidget;

    struct FixtureCase
    {
        const char* name;
        std::size_t eventIndex;
        const char* moduleId;
    };
    const FixtureCase cases[] = {
        {"mvme_workspace_v3.vme", 1U, "module_fixture_target_v3"},
        {"mvme_workspace_v4.vme", 0U, "module_fixture_target_v4"},
    };

    for (const auto& fixture : cases)
    {
        CAPTURE(fixture.name);
        const auto workspace = LoadFixture(fixture.name);
        for (const auto* spelling : {"0x1100", "1100", "0x11000000"})
        {
            CAPTURE(spelling);
            const auto found = workspace.FindEnabledMdpp32ScpTarget(
                Address(spelling));
            REQUIRE(found.status == MvmeWorkspaceTargetStatus::Found);
            REQUIRE(found.target.has_value());
            CHECK(found.target->eventIndex == fixture.eventIndex);
            CHECK(found.target->moduleIndex == 0U);
            CHECK(found.target->address.FullA32Value() == 0x11000000U);
            CHECK(found.target->moduleId == fixture.moduleId);
        }
    }
}

// MVME src/vme_config.cc at fe90d3acd9d6a69aed7eb03ef63282446e54b592
// serializes baseAddress as a JSON number and deserializes it numerically.
TEST_CASE("workspace target lookup accepts the numeric A32 boundaries")
{
    using namespace fidget;

    auto document = LoadFixture("mvme_workspace_v4.vme").Json();
    auto& module = document["DAQConfig"]["events"][0]["modules"][0];

    module["baseAddress"] = 0;
    auto found = Reparse(document).FindEnabledMdpp32ScpTarget(
        Address("0x0000"));
    REQUIRE(found.status == MvmeWorkspaceTargetStatus::Found);
    REQUIRE(found.target.has_value());
    CHECK(found.target->address.FullA32Value() == 0U);

    module["baseAddress"] = 0xFFFF0000ULL;
    found = Reparse(document).FindEnabledMdpp32ScpTarget(
        Address("0xFFFF0000"));
    REQUIRE(found.status == MvmeWorkspaceTargetStatus::Found);
    REQUIRE(found.target.has_value());
    CHECK(found.target->address.FullA32Value() == 0xFFFF0000U);

    module["baseAddress"] = 285212672.0;
    found = Reparse(document).FindEnabledMdpp32ScpTarget(
        Address("0x11000000"));
    CHECK(found.status == MvmeWorkspaceTargetStatus::Found);
}

TEST_CASE("workspace target lookup rejects string baseAddress schema forms")
{
    using namespace fidget;

    struct FixtureCase
    {
        const char* name;
        std::size_t eventIndex;
    };
    const FixtureCase fixtures[] = {
        {"mvme_workspace_v3.vme", 1U},
        {"mvme_workspace_v4.vme", 0U},
    };

    for (const auto& fixture : fixtures)
    {
        for (const auto* text : {"0x1100", "1100", "0x11000000"})
        {
            CAPTURE(fixture.name);
            CAPTURE(text);
            auto document = LoadFixture(fixture.name).Json();
            document["DAQConfig"]["events"][fixture.eventIndex]
                    ["modules"][0]["baseAddress"] = text;
            const auto found = Reparse(document)
                .FindEnabledMdpp32ScpTarget(Address("0x1100"));
            CHECK(found.status
                  == MvmeWorkspaceTargetStatus::InvalidModuleAddress);
            CHECK_FALSE(found.target.has_value());
            CHECK(found.message.find(
                      "workspace schema expects baseAddress to be a numeric "
                      "A32 address")
                  != std::string::npos);
        }
    }
}

TEST_CASE("workspace target lookup rejects duplicate enabled targets")
{
    using namespace fidget;

    auto document = LoadFixture("mvme_workspace_v4.vme").Json();
    auto duplicate = document["DAQConfig"]["events"][0]["modules"][0];
    duplicate["id"] = "module_fixture_duplicate";
    document["DAQConfig"]["events"][1]["modules"].push_back(duplicate);

    const auto workspace = Reparse(document);
    const auto found = workspace.FindEnabledMdpp32ScpTarget(
        Address("0x1100"));
    CHECK(found.status == MvmeWorkspaceTargetStatus::DuplicateTarget);
    CHECK_FALSE(found.target.has_value());
    CHECK(found.message.find("More than one enabled module")
          != std::string::npos);
}

TEST_CASE("workspace target lookup rejects conflicting types at one address")
{
    using namespace fidget;

    auto document = LoadFixture("mvme_workspace_v4.vme").Json();
    auto conflicting = document["DAQConfig"]["events"][0]["modules"][0];
    conflicting["id"] = "module_fixture_conflicting";
    conflicting["type"] = "different_module";
    document["DAQConfig"]["events"][1]["modules"].push_back(conflicting);

    const auto found = Reparse(document).FindEnabledMdpp32ScpTarget(
        Address("0x1100"));
    CHECK(found.status == MvmeWorkspaceTargetStatus::AmbiguousModuleType);
    CHECK_FALSE(found.target.has_value());
    CHECK(found.message.find("do not provide one unambiguous")
          != std::string::npos);
}

TEST_CASE("workspace target lookup rejects ambiguous and wrong module types")
{
    using namespace fidget;

    auto document = LoadFixture("mvme_workspace_v4.vme").Json();
    auto& module = document["DAQConfig"]["events"][0]["modules"][0];

    SUBCASE("missing primary type")
    {
        module.erase("type");
        const auto found = Reparse(document).FindEnabledMdpp32ScpTarget(
            Address("0x1100"));
        CHECK(found.status == MvmeWorkspaceTargetStatus::AmbiguousModuleType);
    }

    SUBCASE("conflicting metadata type")
    {
        module["ModuleMeta"] = {
            {"typeName", "different_module"},
            {"displayName", "Different Module"},
        };
        const auto found = Reparse(document).FindEnabledMdpp32ScpTarget(
            Address("0x1100"));
        CHECK(found.status == MvmeWorkspaceTargetStatus::AmbiguousModuleType);
    }

    SUBCASE("explicit different type")
    {
        module["type"] = "different_module";
        const auto found = Reparse(document).FindEnabledMdpp32ScpTarget(
            Address("0x1100"));
        CHECK(found.status == MvmeWorkspaceTargetStatus::WrongModuleType);
    }
}

TEST_CASE("workspace target lookup fails closed on malformed enabled modules")
{
    using namespace fidget;

    auto document = LoadFixture("mvme_workspace_v4.vme").Json();
    auto& module = document["DAQConfig"]["events"][0]["modules"][0];

    SUBCASE("misaligned full address")
    {
        module["baseAddress"] = 285212673;
        const auto found = Reparse(document).FindEnabledMdpp32ScpTarget(
            Address("0x1100"));
        CHECK(found.status == MvmeWorkspaceTargetStatus::InvalidModuleAddress);
    }

    SUBCASE("numeric shorthand is not a workspace schema form")
    {
        module["baseAddress"] = 0x1100;
        const auto found = Reparse(document).FindEnabledMdpp32ScpTarget(
            Address("0x1100"));
        CHECK(found.status == MvmeWorkspaceTargetStatus::InvalidModuleAddress);
        CHECK(found.message.find("64-KiB aligned") != std::string::npos);
    }

    SUBCASE("non Boolean module enabled field")
    {
        module["enabled"] = "yes";
        const auto found = Reparse(document).FindEnabledMdpp32ScpTarget(
            Address("0x1100"));
        CHECK(found.status == MvmeWorkspaceTargetStatus::MalformedWorkspace);
    }

    SUBCASE("disabled target is not selected")
    {
        module["enabled"] = false;
        const auto found = Reparse(document).FindEnabledMdpp32ScpTarget(
            Address("0x1100"));
        CHECK(found.status == MvmeWorkspaceTargetStatus::NotFound);
    }
}

TEST_CASE("workspace parser rejects malformed and unsupported documents")
{
    using namespace fidget;

    SUBCASE("malformed JSON")
    {
        const auto parsed = ParseMvmeWorkspace("{not-json");
        CHECK_FALSE(parsed.success);
        CHECK(parsed.message.find("not valid JSON") != std::string::npos);
    }

    SUBCASE("non object root")
    {
        const auto parsed = ParseMvmeWorkspace("[]");
        CHECK_FALSE(parsed.success);
        CHECK(parsed.message.find("root is not an object")
              != std::string::npos);
    }

    SUBCASE("missing DAQConfig")
    {
        const auto parsed = ParseMvmeWorkspace("{\"other\":{}}");
        CHECK_FALSE(parsed.success);
        CHECK(parsed.message.find("no DAQConfig") != std::string::npos);
    }

    SUBCASE("unsupported schema")
    {
        const auto parsed = ParseMvmeWorkspace(
            R"({"DAQConfig":{"properties":{"version":5},"events":[]}})");
        CHECK_FALSE(parsed.success);
        CHECK(parsed.message.find("not supported") != std::string::npos);
    }

    SUBCASE("non integer schema")
    {
        const auto parsed = ParseMvmeWorkspace(
            R"({"DAQConfig":{"properties":{"version":"4"},"events":[]}})");
        CHECK_FALSE(parsed.success);
        CHECK(parsed.message.find("not an integer") != std::string::npos);
    }

    SUBCASE("events is not an array")
    {
        const auto parsed = ParseMvmeWorkspace(
            R"({"DAQConfig":{"properties":{"version":4},"events":{}}})");
        CHECK_FALSE(parsed.success);
        CHECK(parsed.message.find("events is not an array")
              != std::string::npos);
    }

    SUBCASE("event modules is not an array")
    {
        const auto parsed = ParseMvmeWorkspace(
            R"({"DAQConfig":{"properties":{"version":3},"events":[{"modules":{}}]}})");
        CHECK_FALSE(parsed.success);
        CHECK(parsed.message.find("has no modules array")
              != std::string::npos);
    }
}
