#ifndef FIDGET_CORE_MVME_EXPORT_H
#define FIDGET_CORE_MVME_EXPORT_H

#include "core/ScpProfile.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace fidget {

struct MvmeExportResult
{
    bool success = false;
    std::string message;
    std::string text;
    std::uint64_t sourceProfileChecksum = 0U;
    std::size_t valueCount = 0U;
};

[[nodiscard]] MvmeExportResult GenerateFw2051MvmeScript(
    const ScpProfile& profile,
    const std::string& generatedTimestamp);

} // namespace fidget

#endif
