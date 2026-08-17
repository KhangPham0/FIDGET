#include "core/ActivityLogFile.h"

#include <fstream>

namespace fidget {

std::string ProjectActivityLogPath(const std::string& projectPath)
{
    return projectPath.empty() ? std::string{} : projectPath + ".activity";
}

ActivityLogAppendResult AppendActivityLogEntry(
    const std::string& path,
    const ActivityLogEntry& entry)
{
    ActivityLogAppendResult result;
    if (path.empty())
    {
        result.message = "The activity-log path is empty.";
        return result;
    }

    std::ofstream output(path, std::ios::binary | std::ios::app);
    if (!output)
    {
        result.message = "Could not open the activity log for append: "
            + path;
        return result;
    }

    output << FormatActivityLogEntry(entry) << '\n';
    output.flush();
    if (!output)
    {
        result.message = "Could not flush the activity log: " + path;
        return result;
    }

    result.success = true;
    result.message = "Appended the activity log: " + path;
    return result;
}

} // namespace fidget
