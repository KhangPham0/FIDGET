#ifndef FIDGET_CORE_ACTIVITY_LOG_FILE_H
#define FIDGET_CORE_ACTIVITY_LOG_FILE_H

#include "core/ActivityLog.h"

#include <string>

namespace fidget {

struct ActivityLogAppendResult
{
    bool success = false;
    std::string message;
};

[[nodiscard]] std::string ProjectActivityLogPath(
    const std::string& projectPath);

[[nodiscard]] ActivityLogAppendResult AppendActivityLogEntry(
    const std::string& path,
    const ActivityLogEntry& entry);

} // namespace fidget

#endif
