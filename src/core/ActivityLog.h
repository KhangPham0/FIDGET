#ifndef FIDGET_CORE_ACTIVITY_LOG_H
#define FIDGET_CORE_ACTIVITY_LOG_H

#include "core/TunerStatus.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fidget {

inline constexpr std::size_t ActivityLogEntryLimit = 500U;

enum class ActivityLogCategory
{
    Session,
    Audit,
    Capture,
    Apply,
    Startup,
    Acquisition,
    Source,
    Preview,
    Recovery,
    Export,
};

struct ActivityParameterChange
{
    std::uint16_t registerOffset = 0U;
    std::uint16_t quad = 0U;
    std::uint16_t before = 0U;
    std::uint16_t after = 0U;
};

struct ActivityLogEntry
{
    std::chrono::system_clock::time_point timestamp;
    ActivityLogCategory category = ActivityLogCategory::Session;
    TunerStatusLevel severity = TunerStatusLevel::Information;
    std::string summary;
    std::optional<ActivityParameterChange> parameterChange;
};

class ActivityLog
{
public:
    void Append(ActivityLogEntry entry);

    [[nodiscard]] bool Empty() const noexcept;
    [[nodiscard]] std::size_t Size() const noexcept;
    [[nodiscard]] const std::vector<ActivityLogEntry>& Entries()
        const noexcept;

private:
    std::vector<ActivityLogEntry> entries_;
};

[[nodiscard]] const char* ActivityLogCategoryName(
    ActivityLogCategory category) noexcept;

[[nodiscard]] const char* TunerStatusLevelName(
    TunerStatusLevel level) noexcept;

[[nodiscard]] std::string FormatActivityLogEntry(
    const ActivityLogEntry& entry);

} // namespace fidget

#endif
