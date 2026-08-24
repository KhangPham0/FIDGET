#ifndef FIDGET_CORE_MVME_WORKSPACE_H
#define FIDGET_CORE_MVME_WORKSPACE_H

#include "core/TargetModuleAddress.h"

#include "nlohmann/json.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace fidget {

enum class MvmeWorkspaceSchemaVersion
{
    V3 = 3,
    V4 = 4,
};

enum class MvmeWorkspaceTargetStatus
{
    Found,
    NotFound,
    DuplicateTarget,
    AmbiguousModuleType,
    WrongModuleType,
    InvalidModuleAddress,
    MalformedWorkspace,
};

struct MvmeWorkspaceTarget
{
    std::size_t eventIndex;
    std::size_t moduleIndex;
    TargetModuleAddress address;
    std::string eventId;
    std::string moduleId;
};

struct MvmeWorkspaceTargetLookupResult
{
    MvmeWorkspaceTargetStatus status = MvmeWorkspaceTargetStatus::NotFound;
    std::string message;
    std::optional<MvmeWorkspaceTarget> target;
};

struct MvmeWorkspaceParseResult;

// Passive representation of an MVME workspace. Parsing and target lookup do
// not contact hardware and do not establish controller identity, idleness, or
// ownership. The ordered JSON document is retained so unknown fields, scripts,
// objects, identifiers, events, and source ordering survive serialization.
class MvmeWorkspace
{
public:
    using JsonDocument = nlohmann::ordered_json;

    MvmeWorkspace(const MvmeWorkspace&) = default;
    MvmeWorkspace& operator=(const MvmeWorkspace&) = default;
    MvmeWorkspace(MvmeWorkspace&&) noexcept = default;
    MvmeWorkspace& operator=(MvmeWorkspace&&) noexcept = default;

    [[nodiscard]] MvmeWorkspaceSchemaVersion SchemaVersion() const noexcept;
    [[nodiscard]] const JsonDocument& Json() const noexcept;
    [[nodiscard]] std::string Serialize(int indentation = 2) const;

    [[nodiscard]] MvmeWorkspaceTargetLookupResult
    FindEnabledMdpp32ScpTarget(TargetModuleAddress address) const;

private:
    MvmeWorkspace(
        JsonDocument document,
        MvmeWorkspaceSchemaVersion schemaVersion);

    JsonDocument document_;
    MvmeWorkspaceSchemaVersion schemaVersion_;

    friend MvmeWorkspaceParseResult ParseMvmeWorkspace(
        std::string_view text);
};

struct MvmeWorkspaceParseResult
{
    bool success = false;
    std::string message;
    std::optional<MvmeWorkspace> workspace;
};

// Supported MVME workspace schemas store their version at
// DAQConfig.properties.version. Versions 3 and 4 are accepted.
[[nodiscard]] MvmeWorkspaceParseResult ParseMvmeWorkspace(
    std::string_view text);

} // namespace fidget

#endif
