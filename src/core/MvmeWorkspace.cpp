#include "core/MvmeWorkspace.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <system_error>
#include <utility>
#include <vector>

namespace fidget {
namespace {

constexpr std::string_view Mdpp32ScpType = "mdpp32_scp";

MvmeWorkspaceParseResult ParseFailure(std::string message)
{
    MvmeWorkspaceParseResult result;
    result.message = std::move(message);
    return result;
}

MvmeWorkspaceTargetLookupResult LookupFailure(
    const MvmeWorkspaceTargetStatus status,
    std::string message)
{
    MvmeWorkspaceTargetLookupResult result;
    result.status = status;
    result.message = std::move(message);
    return result;
}

std::string IndexedField(
    const std::size_t eventIndex,
    const std::size_t moduleIndex,
    std::string_view detail)
{
    return "Event " + std::to_string(eventIndex)
        + ", module " + std::to_string(moduleIndex) + ": "
        + std::string(detail);
}

TargetModuleAddressParseResult ParseNumericModuleAddress(
    const std::uint32_t value)
{
    std::array<char, 10U> text{};
    text[0] = '0';
    text[1] = 'x';
    const auto converted = std::to_chars(
        text.data() + 2U, text.data() + text.size(), value, 16);
    if (converted.ec != std::errc{})
    {
        TargetModuleAddressParseResult result;
        result.message = "Could not format the numeric module address.";
        return result;
    }

    return ParseTargetModuleAddress(
        std::string_view(text.data(), converted.ptr - text.data()));
}

TargetModuleAddressParseResult ParseModuleAddress(
    const nlohmann::ordered_json& value)
{
    if (value.is_string())
        return ParseTargetModuleAddress(value.get_ref<const std::string&>());

    if (value.is_number_unsigned())
    {
        const auto number = value.get<std::uint64_t>();
        if (number > std::numeric_limits<std::uint32_t>::max())
        {
            TargetModuleAddressParseResult result;
            result.message = "The module address exceeds the A32 range.";
            return result;
        }
        return ParseNumericModuleAddress(static_cast<std::uint32_t>(number));
    }

    if (value.is_number_integer())
    {
        const auto number = value.get<std::int64_t>();
        if (number < 0
            || static_cast<std::uint64_t>(number)
                > std::numeric_limits<std::uint32_t>::max())
        {
            TargetModuleAddressParseResult result;
            result.message = "The module address is outside the A32 range.";
            return result;
        }
        return ParseNumericModuleAddress(static_cast<std::uint32_t>(number));
    }

    TargetModuleAddressParseResult result;
    result.message = "The module address is neither a number nor text.";
    return result;
}

std::string OptionalString(
    const nlohmann::ordered_json& object,
    const char* key)
{
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string())
        return {};
    return found->get<std::string>();
}

MvmeWorkspaceTargetLookupResult ValidateTargetType(
    const nlohmann::ordered_json& module,
    const MvmeWorkspaceTarget& target)
{
    const auto type = module.find("type");
    if (type == module.end() || !type->is_string())
    {
        return LookupFailure(
            MvmeWorkspaceTargetStatus::AmbiguousModuleType,
            IndexedField(
                target.eventIndex,
                target.moduleIndex,
                "the enabled target has no unambiguous module type."));
    }

    const auto primaryType = type->get<std::string>();
    const auto metadata = module.find("ModuleMeta");
    if (metadata != module.end())
    {
        if (!metadata->is_object())
        {
            return LookupFailure(
                MvmeWorkspaceTargetStatus::AmbiguousModuleType,
                IndexedField(
                    target.eventIndex,
                    target.moduleIndex,
                    "ModuleMeta is not an object."));
        }

        const auto metadataType = metadata->find("typeName");
        if (metadataType != metadata->end()
            && (!metadataType->is_string()
                || metadataType->get<std::string>() != primaryType))
        {
            return LookupFailure(
                MvmeWorkspaceTargetStatus::AmbiguousModuleType,
                IndexedField(
                    target.eventIndex,
                    target.moduleIndex,
                    "the primary and metadata module types disagree."));
        }
    }

    if (primaryType != Mdpp32ScpType)
    {
        return LookupFailure(
            MvmeWorkspaceTargetStatus::WrongModuleType,
            IndexedField(
                target.eventIndex,
                target.moduleIndex,
                "the enabled module is not an mdpp32_scp target."));
    }

    MvmeWorkspaceTargetLookupResult result;
    result.status = MvmeWorkspaceTargetStatus::Found;
    result.message = "Found one enabled mdpp32_scp target.";
    result.target = target;
    return result;
}

} // namespace

MvmeWorkspace::MvmeWorkspace(
    JsonDocument document,
    const MvmeWorkspaceSchemaVersion schemaVersion)
    : document_(std::move(document))
    , schemaVersion_(schemaVersion)
{
}

MvmeWorkspaceSchemaVersion MvmeWorkspace::SchemaVersion() const noexcept
{
    return schemaVersion_;
}

const MvmeWorkspace::JsonDocument& MvmeWorkspace::Json() const noexcept
{
    return document_;
}

std::string MvmeWorkspace::Serialize(const int indentation) const
{
    return document_.dump(indentation) + '\n';
}

MvmeWorkspaceTargetLookupResult
MvmeWorkspace::FindEnabledMdpp32ScpTarget(
    const TargetModuleAddress address) const
{
    const auto& events = document_.at("DAQConfig").at("events");
    std::vector<MvmeWorkspaceTarget> matchingModules;

    for (std::size_t eventIndex = 0U; eventIndex < events.size(); ++eventIndex)
    {
        const auto& event = events[eventIndex];
        const auto eventEnabled = event.find("enabled");
        if (eventEnabled == event.end() || !eventEnabled->is_boolean())
        {
            return LookupFailure(
                MvmeWorkspaceTargetStatus::MalformedWorkspace,
                "Event " + std::to_string(eventIndex)
                    + " has no Boolean enabled field.");
        }
        if (!eventEnabled->get<bool>())
            continue;

        const auto& modules = event.at("modules");
        for (std::size_t moduleIndex = 0U;
             moduleIndex < modules.size();
             ++moduleIndex)
        {
            const auto& module = modules[moduleIndex];
            if (!module.is_object())
            {
                return LookupFailure(
                    MvmeWorkspaceTargetStatus::MalformedWorkspace,
                    IndexedField(
                        eventIndex,
                        moduleIndex,
                        "the module entry is not an object."));
            }

            const auto moduleEnabled = module.find("enabled");
            if (moduleEnabled == module.end()
                || !moduleEnabled->is_boolean())
            {
                return LookupFailure(
                    MvmeWorkspaceTargetStatus::MalformedWorkspace,
                    IndexedField(
                        eventIndex,
                        moduleIndex,
                        "the module has no Boolean enabled field."));
            }
            if (!moduleEnabled->get<bool>())
                continue;

            const auto baseAddress = module.find("baseAddress");
            if (baseAddress == module.end())
            {
                return LookupFailure(
                    MvmeWorkspaceTargetStatus::InvalidModuleAddress,
                    IndexedField(
                        eventIndex,
                        moduleIndex,
                        "the enabled module has no baseAddress."));
            }

            const auto parsedAddress = ParseModuleAddress(*baseAddress);
            if (!parsedAddress.success || !parsedAddress.address.has_value())
            {
                return LookupFailure(
                    MvmeWorkspaceTargetStatus::InvalidModuleAddress,
                    IndexedField(
                        eventIndex,
                        moduleIndex,
                        parsedAddress.message));
            }
            if (*parsedAddress.address != address)
                continue;

            matchingModules.push_back(MvmeWorkspaceTarget{
                eventIndex,
                moduleIndex,
                *parsedAddress.address,
                OptionalString(event, "id"),
                OptionalString(module, "id"),
            });
        }
    }

    if (matchingModules.empty())
    {
        return LookupFailure(
            MvmeWorkspaceTargetStatus::NotFound,
            "No enabled module uses the requested normalized address.");
    }
    if (matchingModules.size() != 1U)
    {
        for (const auto& target : matchingModules)
        {
            const auto type = ValidateTargetType(
                events[target.eventIndex]["modules"][target.moduleIndex],
                target);
            if (type.status != MvmeWorkspaceTargetStatus::Found)
            {
                return LookupFailure(
                    MvmeWorkspaceTargetStatus::AmbiguousModuleType,
                    "Enabled modules at the requested normalized address "
                    "do not provide one unambiguous mdpp32_scp type.");
            }
        }

        return LookupFailure(
            MvmeWorkspaceTargetStatus::DuplicateTarget,
            "More than one enabled module uses the requested normalized "
            "address across the workspace events.");
    }

    const auto& target = matchingModules.front();
    return ValidateTargetType(
        events[target.eventIndex]["modules"][target.moduleIndex], target);
}

MvmeWorkspaceParseResult ParseMvmeWorkspace(const std::string_view text)
{
    MvmeWorkspace::JsonDocument document;
    try
    {
        document = MvmeWorkspace::JsonDocument::parse(
            text.begin(), text.end());
    }
    catch (const nlohmann::json::parse_error& error)
    {
        return ParseFailure(
            "The MVME workspace is not valid JSON: "
            + std::string(error.what()));
    }

    if (!document.is_object())
        return ParseFailure("The MVME workspace root is not an object.");

    const auto daqConfig = document.find("DAQConfig");
    if (daqConfig == document.end() || !daqConfig->is_object())
        return ParseFailure("The MVME workspace has no DAQConfig object.");

    const auto properties = daqConfig->find("properties");
    if (properties == daqConfig->end() || !properties->is_object())
    {
        return ParseFailure(
            "The MVME workspace has no DAQConfig properties object.");
    }

    const auto version = properties->find("version");
    if (version == properties->end()
        || (!version->is_number_integer()
            && !version->is_number_unsigned()))
    {
        return ParseFailure(
            "The MVME workspace schema version is missing or not an "
            "integer.");
    }

    MvmeWorkspaceSchemaVersion schemaVersion;
    if (*version == 3)
        schemaVersion = MvmeWorkspaceSchemaVersion::V3;
    else if (*version == 4)
        schemaVersion = MvmeWorkspaceSchemaVersion::V4;
    else
    {
        return ParseFailure(
            "The MVME workspace schema version is not supported.");
    }

    const auto events = daqConfig->find("events");
    if (events == daqConfig->end() || !events->is_array())
        return ParseFailure("DAQConfig.events is not an array.");

    for (std::size_t eventIndex = 0U;
         eventIndex < events->size();
         ++eventIndex)
    {
        const auto& event = (*events)[eventIndex];
        if (!event.is_object())
        {
            return ParseFailure(
                "Event " + std::to_string(eventIndex)
                + " is not an object.");
        }
        const auto modules = event.find("modules");
        if (modules == event.end() || !modules->is_array())
        {
            return ParseFailure(
                "Event " + std::to_string(eventIndex)
                + " has no modules array.");
        }
    }

    MvmeWorkspaceParseResult result;
    result.success = true;
    result.message = "Parsed the MVME workspace without hardware access.";
    result.workspace = MvmeWorkspace(std::move(document), schemaVersion);
    return result;
}

} // namespace fidget
