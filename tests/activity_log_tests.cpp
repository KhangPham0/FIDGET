#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/ActivityLog.h"
#include "core/ActivityLogFile.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

TEST_CASE("activity log retains insertion order")
{
    using namespace fidget;

    ActivityLog log;
    log.Append({{}, ActivityLogCategory::Session,
                TunerStatusLevel::Information, "first", std::nullopt});
    log.Append({{}, ActivityLogCategory::Audit,
                TunerStatusLevel::Success, "second", std::nullopt});

    REQUIRE(log.Size() == 2U);
    CHECK(log.Entries()[0].summary == "first");
    CHECK(log.Entries()[1].summary == "second");
}

TEST_CASE("activity log drops the oldest entry at its bound")
{
    using namespace fidget;

    ActivityLog log;
    for (std::size_t index = 0U; index < ActivityLogEntryLimit + 3U;
         ++index)
    {
        log.Append({{}, ActivityLogCategory::Session,
                    TunerStatusLevel::Information,
                    std::to_string(index), std::nullopt});
    }

    REQUIRE(log.Size() == ActivityLogEntryLimit);
    CHECK(log.Entries().front().summary == "3");
    CHECK(log.Entries().back().summary
          == std::to_string(ActivityLogEntryLimit + 2U));
}

TEST_CASE("activity formatter names every category")
{
    using namespace fidget;

    const std::array<std::pair<ActivityLogCategory, const char*>, 10>
        categories{{
            {ActivityLogCategory::Session, "session"},
            {ActivityLogCategory::Audit, "audit"},
            {ActivityLogCategory::Capture, "capture"},
            {ActivityLogCategory::Apply, "apply"},
            {ActivityLogCategory::Startup, "startup"},
            {ActivityLogCategory::Acquisition, "acquisition"},
            {ActivityLogCategory::Source, "source"},
            {ActivityLogCategory::Preview, "preview"},
            {ActivityLogCategory::Recovery, "recovery"},
            {ActivityLogCategory::Export, "export"},
        }};

    for (const auto& category : categories)
    {
        const auto line = FormatActivityLogEntry(
            {{}, category.first, TunerStatusLevel::Information,
             "message", std::nullopt});
        CHECK(line.find("[" + std::string(category.second) + "]")
              != std::string::npos);
    }
}

TEST_CASE("activity formatter produces one line with parameter details")
{
    using namespace fidget;

    const auto line = FormatActivityLogEntry({
        std::chrono::system_clock::time_point{},
        ActivityLogCategory::Apply,
        TunerStatusLevel::Success,
        "Gain applied\nwith verification",
        ActivityParameterChange{0x611AU, 7U, 250U, 200U},
    });

    CHECK(line
          == "1970-01-01T00:00:00Z [apply] [success] Gain applied\\nwith verification register=0x611A quad=7 before=250 after=200");
}

TEST_CASE("project activity files append and flush complete lines")
{
    using namespace fidget;

    const auto projectPath = (
        std::filesystem::temp_directory_path()
        / "fidget-activity-log-test.mwwcrate").string();
    const auto activityPath = ProjectActivityLogPath(projectPath);
    std::remove(activityPath.c_str());

    const ActivityLogEntry first{
        {}, ActivityLogCategory::Session, TunerStatusLevel::Success,
        "project opened", std::nullopt};
    const ActivityLogEntry second{
        {}, ActivityLogCategory::Audit, TunerStatusLevel::Warning,
        "audit found a warning", std::nullopt};
    REQUIRE(AppendActivityLogEntry(activityPath, first).success);
    REQUIRE(AppendActivityLogEntry(activityPath, second).success);

    std::ifstream input(activityPath, std::ios::binary);
    REQUIRE(input.good());
    std::string firstLine;
    std::string secondLine;
    REQUIRE(static_cast<bool>(std::getline(input, firstLine)));
    REQUIRE(static_cast<bool>(std::getline(input, secondLine)));
    CHECK(firstLine == FormatActivityLogEntry(first));
    CHECK(secondLine == FormatActivityLogEntry(second));
    std::string trailing;
    CHECK_FALSE(static_cast<bool>(std::getline(input, trailing)));

    std::remove(activityPath.c_str());
}

TEST_CASE("empty project paths do not name an activity file")
{
    using namespace fidget;

    CHECK(ProjectActivityLogPath("").empty());
    CHECK(ProjectActivityLogPath("two-scp-crate.mwwcrate")
          == "two-scp-crate.mwwcrate.activity");
}
