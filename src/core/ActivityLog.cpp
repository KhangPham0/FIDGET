#include "core/ActivityLog.h"

#include <ctime>
#include <iomanip>
#include <sstream>
#include <utility>

namespace fidget {
namespace {

std::string EscapeSingleLine(const std::string& text)
{
    std::string escaped;
    escaped.reserve(text.size());
    for (const char character : text)
    {
        switch (character)
        {
        case '\\':
            escaped += "\\\\";
            break;
        case '\n':
            escaped += "\\n";
            break;
        case '\r':
            escaped += "\\r";
            break;
        case '\t':
            escaped += "\\t";
            break;
        default:
            escaped += character;
            break;
        }
    }
    return escaped;
}

std::string FormatTimestamp(
    const std::chrono::system_clock::time_point timestamp)
{
    const std::time_t time = std::chrono::system_clock::to_time_t(timestamp);
    std::tm utc{};
    if (gmtime_r(&time, &utc) == nullptr)
    {
        return "invalid-time";
    }

    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

} // namespace

void ActivityLog::Append(ActivityLogEntry entry)
{
    if (entries_.size() == ActivityLogEntryLimit)
    {
        entries_.erase(entries_.begin());
    }
    entries_.push_back(std::move(entry));
}

bool ActivityLog::Empty() const noexcept
{
    return entries_.empty();
}

std::size_t ActivityLog::Size() const noexcept
{
    return entries_.size();
}

const std::vector<ActivityLogEntry>& ActivityLog::Entries() const noexcept
{
    return entries_;
}

const char* ActivityLogCategoryName(
    const ActivityLogCategory category) noexcept
{
    switch (category)
    {
    case ActivityLogCategory::Session:
        return "session";
    case ActivityLogCategory::Audit:
        return "audit";
    case ActivityLogCategory::Capture:
        return "capture";
    case ActivityLogCategory::Apply:
        return "apply";
    case ActivityLogCategory::Startup:
        return "startup";
    case ActivityLogCategory::Acquisition:
        return "acquisition";
    case ActivityLogCategory::Source:
        return "source";
    case ActivityLogCategory::Preview:
        return "preview";
    case ActivityLogCategory::Recovery:
        return "recovery";
    case ActivityLogCategory::Export:
        return "export";
    }
    return "unknown";
}

const char* TunerStatusLevelName(const TunerStatusLevel level) noexcept
{
    switch (level)
    {
    case TunerStatusLevel::Information:
        return "info";
    case TunerStatusLevel::Success:
        return "success";
    case TunerStatusLevel::Warning:
        return "warning";
    case TunerStatusLevel::Error:
        return "error";
    }
    return "unknown";
}

std::string FormatActivityLogEntry(const ActivityLogEntry& entry)
{
    std::ostringstream output;
    output << FormatTimestamp(entry.timestamp)
           << " [" << ActivityLogCategoryName(entry.category) << ']'
           << " [" << TunerStatusLevelName(entry.severity) << "] "
           << EscapeSingleLine(entry.summary);
    if (entry.parameterChange)
    {
        const auto& change = *entry.parameterChange;
        output << " register=0x"
               << std::uppercase << std::hex << std::setw(4)
               << std::setfill('0') << change.registerOffset
               << std::dec << std::nouppercase
               << " quad=" << change.quad
               << " before=" << change.before
               << " after=" << change.after;
    }
    return output.str();
}

} // namespace fidget
