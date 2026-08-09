#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/CrateProject.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

namespace {

fidget::CrateProject MakeProject()
{
    using namespace fidget;

    CrateProject project;
    project.mvlcHost = "mvlc-test";
    project.mvlcCommandPort = 32768U;
    project.streamHost = "stream-test";
    project.streamPort = 42333U;
    project.modules = {
        {"MDPP 1", 0x11000000U, MdppBackend::Scp,
         "profiles/mdpp1_scp_profile.mwwscp"},
        {"MDPP 2", 0x33330000U, MdppBackend::Scp,
         "profiles/mdpp2_scp_profile.mwwscp"},
        {"QDC test", 0x44000000U, MdppBackend::Qdc,
         "profiles/qdc_test_profile.mwwqdc"},
    };
    return project;
}

std::string ReadFixture(const char* name)
{
    const std::string path =
        std::string(FIDGET_TEST_FIXTURE_DIR) + '/' + name;
    std::ifstream input(path, std::ios::binary);
    REQUIRE(input.good());
    return std::string(std::istreambuf_iterator<char>(input),
                       std::istreambuf_iterator<char>());
}

std::string MakeTemporaryPath(const char* suffix)
{
    const char* temporaryDirectory = std::getenv("TMPDIR");
    if (temporaryDirectory == nullptr || temporaryDirectory[0] == '\0')
    {
        temporaryDirectory = "/tmp";
    }

    const auto unique = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    return std::string(temporaryDirectory) + "/fidget_crate_project_"
        + std::to_string(unique) + suffix;
}

} // namespace

TEST_CASE("a crate project round-trips through its string format")
{
    using namespace fidget;

    const auto project = MakeProject();
    const auto serialized = SerializeCrateProject(project);
    REQUIRE(serialized.success);

    const auto parsed = ParseCrateProject(serialized.text);
    REQUIRE(parsed.success);
    REQUIRE(parsed.project.has_value());
    CHECK(parsed.project->mvlcHost == project.mvlcHost);
    CHECK(parsed.project->mvlcCommandPort == project.mvlcCommandPort);
    CHECK(parsed.project->streamHost == project.streamHost);
    CHECK(parsed.project->streamPort == project.streamPort);
    REQUIRE(parsed.project->modules.size() == project.modules.size());

    for (std::size_t index = 0; index < project.modules.size(); ++index)
    {
        const auto& expected = project.modules[index];
        const auto& actual = parsed.project->modules[index];
        CHECK(actual.name == expected.name);
        CHECK(actual.baseAddress == expected.baseAddress);
        CHECK(actual.backend == expected.backend);
        CHECK(actual.profilePath == expected.profilePath);
    }

    CHECK(MdppBackendImplemented(MdppBackend::Scp));
    CHECK_FALSE(MdppBackendImplemented(MdppBackend::Qdc));
}

TEST_CASE("the real-hardware crate project remains compatible")
{
    using namespace fidget;

    const auto parsed = ParseCrateProject(
        ReadFixture("two-scp-crate.mwwcrate"));
    REQUIRE(parsed.success);
    REQUIRE(parsed.project.has_value());
    CHECK(parsed.project->mvlcHost == "mvlc-test");
    CHECK(parsed.project->mvlcCommandPort == 32768U);
    CHECK(parsed.project->streamHost ==
          "stream-test");
    CHECK(parsed.project->streamPort == 42333U);
    REQUIRE(parsed.project->modules.size() == 2U);
    CHECK(parsed.project->modules[0].name == "MDPP 1");
    CHECK(parsed.project->modules[0].baseAddress == 285212672U);
    CHECK(parsed.project->modules[0].backend == MdppBackend::Scp);
    CHECK(parsed.project->modules[1].baseAddress == 858980352U);
}

TEST_CASE("crate validation rejects ambiguous or unsafe targets")
{
    using namespace fidget;

    auto project = MakeProject();
    project.modules[1].baseAddress = project.modules[0].baseAddress;
    const auto duplicateBase = ValidateCrateProject(project);
    CHECK_FALSE(duplicateBase.success);
    CHECK(duplicateBase.message.find("VME bases must be unique")
          != std::string::npos);

    project = MakeProject();
    project.modules[1].name = project.modules[0].name;
    const auto duplicateName = ValidateCrateProject(project);
    CHECK_FALSE(duplicateName.success);
    CHECK(duplicateName.message.find("module names must be unique")
          != std::string::npos);

    project = MakeProject();
    project.modules[0].baseAddress = 0x11000002U;
    const auto misalignedBase = ValidateCrateProject(project);
    CHECK_FALSE(misalignedBase.success);
    CHECK(misalignedBase.message.find("64 KiB aligned")
          != std::string::npos);

    project = MakeProject();
    project.mvlcHost = "bad host";
    const auto invalidHost = ValidateCrateProject(project);
    CHECK_FALSE(invalidHost.success);
    CHECK(invalidHost.message.find("MVLC command endpoint")
          != std::string::npos);

    project = MakeProject();
    project.streamPort = 0U;
    const auto invalidStream = ValidateCrateProject(project);
    CHECK_FALSE(invalidStream.success);
    CHECK(invalidStream.message.find("Stream Server endpoint")
          != std::string::npos);

    project = MakeProject();
    project.modules.clear();
    const auto emptyInventory = ValidateCrateProject(project);
    CHECK_FALSE(emptyInventory.success);
    CHECK(emptyInventory.message.find("between 1 and 16 modules")
          != std::string::npos);
}

TEST_CASE("crate parsing rejects version, corruption, trailing data, and unknown backends")
{
    using namespace fidget;

    const auto serialized = SerializeCrateProject(MakeProject());
    REQUIRE(serialized.success);

    std::string unsupportedVersion = serialized.text;
    const auto version = unsupportedVersion.find("MWW_CRATE_PROJECT 1");
    REQUIRE(version != std::string::npos);
    unsupportedVersion[version + 18U] = '2';
    const auto versionFailure = ParseCrateProject(unsupportedVersion);
    CHECK_FALSE(versionFailure.success);
    CHECK(versionFailure.message.find("Unsupported crate-project")
          != std::string::npos);

    std::string tampered = serialized.text;
    const auto hostPosition = tampered.find("mvlc-test");
    REQUIRE(hostPosition != std::string::npos);
    tampered[hostPosition + 8U] = '6';
    const auto checksumFailure = ParseCrateProject(tampered);
    CHECK_FALSE(checksumFailure.success);
    CHECK(checksumFailure.message.find("checksum mismatch")
          != std::string::npos);

    const auto trailing = ParseCrateProject(
        serialized.text + "UNEXPECTED\n");
    CHECK_FALSE(trailing.success);
    CHECK(trailing.message.find("unexpected trailing token")
          != std::string::npos);

    const auto unknownBackend = ParseCrateProject(
        "MWW_CRATE_PROJECT 1\n"
        "MVLC \"mvlc-test\" 32768\n"
        "STREAM \"daq-host\" 42333\n"
        "MODULES 1\n"
        "MODULE \"bad\" 285212672 99 \"bad.profile\"\n"
        "CHECKSUM 0\nEND\n");
    CHECK_FALSE(unknownBackend.success);
    CHECK(unknownBackend.message.find("unknown firmware backend")
          != std::string::npos);
}

TEST_CASE("the crate file wrapper atomically replaces an existing project")
{
    using namespace fidget;

    auto project = MakeProject();
    const std::string path = MakeTemporaryPath(".mwwcrate");
    const std::string temporary = path + ".tmp";
    REQUIRE(SaveCrateProject(project, path).success);

    project.mvlcHost = "mvlc-0099";
    project.modules.pop_back();
    REQUIRE(SaveCrateProject(project, path).success);

    const auto loaded = LoadCrateProject(path);
    REQUIRE(loaded.success);
    REQUIRE(loaded.project.has_value());
    CHECK(loaded.project->mvlcHost == "mvlc-0099");
    CHECK(loaded.project->modules.size() == 2U);
    CHECK(std::ifstream(temporary).fail());

    std::remove(path.c_str());
    std::remove(temporary.c_str());
}

TEST_CASE("crate module routing enforces the selected firmware backend")
{
    using namespace fidget;

    auto project = MakeProject();
    project.modules.insert(
        project.modules.begin() + 2,
        {"MDPP 3", 0x55000000U, MdppBackend::Scp,
         "profiles/mdpp3_scp_profile.mwwscp"});

    const auto thirdScp = ResolveCrateProjectModuleTarget(
        project, 2U, MdppBackend::Scp);
    REQUIRE(thirdScp.success);
    REQUIRE(thirdScp.module.has_value());
    CHECK(thirdScp.module->name == "MDPP 3");
    CHECK(thirdScp.module->baseAddress == 0x55000000U);
    CHECK(thirdScp.module->profilePath ==
          "profiles/mdpp3_scp_profile.mwwscp");

    const auto qdcAsScp = ResolveCrateProjectModuleTarget(
        project, 3U, MdppBackend::Scp);
    CHECK_FALSE(qdcAsScp.success);
    CHECK_FALSE(qdcAsScp.module.has_value());
    CHECK(qdcAsScp.message.find("requires SCP") != std::string::npos);

    const auto qdcTarget = ResolveCrateProjectModuleTarget(
        project, 3U, MdppBackend::Qdc);
    REQUIRE(qdcTarget.success);
    REQUIRE(qdcTarget.module.has_value());
    CHECK(qdcTarget.module->backend == MdppBackend::Qdc);

    const auto outOfRange = ResolveCrateProjectModuleTarget(
        project, project.modules.size(), MdppBackend::Scp);
    CHECK_FALSE(outOfRange.success);
    CHECK_FALSE(outOfRange.module.has_value());
}
