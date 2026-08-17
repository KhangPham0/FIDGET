#include "core/MvmeExportFile.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fidget {

MvmeExportSaveResult SaveMvmeExportText(
    const std::string& text,
    const std::string& path,
    const bool allowOverwrite)
{
    MvmeExportSaveResult result;
    if (path.empty())
    {
        result.message = "The MVME export path is empty.";
        return result;
    }

    std::error_code existenceError;
    result.outputAlreadyExists = std::filesystem::exists(
        path, existenceError);
    if (existenceError)
    {
        result.message = "Could not inspect the MVME export path: "
            + existenceError.message();
        return result;
    }
    if (result.outputAlreadyExists && !allowOverwrite)
    {
        result.message = "The MVME export already exists: " + path;
        return result;
    }

    const std::string temporaryPath = path + ".tmp";
    std::ofstream output(
        temporaryPath, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        result.message = "Could not open the temporary MVME export: "
            + temporaryPath;
        return result;
    }
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    output.flush();
    output.close();
    if (!output)
    {
        std::remove(temporaryPath.c_str());
        result.message = "Writing the MVME export failed.";
        return result;
    }
    if (std::rename(temporaryPath.c_str(), path.c_str()) != 0)
    {
        const std::string renameError = std::strerror(errno);
        std::remove(temporaryPath.c_str());
        result.message = "Could not install the completed MVME export: "
            + renameError + '.';
        return result;
    }

    result.success = true;
    result.message = "Saved the MVME settings export to '" + path + "'.";
    return result;
}

} // namespace fidget
