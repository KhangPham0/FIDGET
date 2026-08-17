#ifndef FIDGET_CORE_MVME_EXPORT_FILE_H
#define FIDGET_CORE_MVME_EXPORT_FILE_H

#include <string>

namespace fidget {

struct MvmeExportSaveResult
{
    bool success = false;
    bool outputAlreadyExists = false;
    std::string message;
};

[[nodiscard]] MvmeExportSaveResult SaveMvmeExportText(
    const std::string& text,
    const std::string& path,
    bool allowOverwrite);

} // namespace fidget

#endif
