#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/ActivityLog.h"

#include <array>
#include <chrono>
#include <cstddef>
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
