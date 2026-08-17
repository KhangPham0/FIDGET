#include "core/MvmeExport.h"

#include "core/ScpRegistry.h"

#include <cstdio>
#include <sstream>
#include <string>

namespace fidget {
namespace {

std::string Hexadecimal16(const std::uint16_t value)
{
    char text[8]{};
    std::snprintf(
        text, sizeof(text), "0x%04X", static_cast<unsigned>(value));
    return text;
}

std::string Hexadecimal32(const std::uint32_t value)
{
    char text[16]{};
    std::snprintf(
        text, sizeof(text), "0x%08X", static_cast<unsigned>(value));
    return text;
}

std::string ValidateExportValues(
    const Fw2051ScpConfigurationSnapshot& configuration)
{
    for (const auto& quad : configuration.quads)
    {
        for (const auto& definition : Fw2051ScpSettingRegistry)
        {
            const auto value = Fw2051ScpQuadRegisterValue(
                quad, definition.registerOffset);
            if (!value)
            {
                return "The FW2051 registry does not map every profile value.";
            }
            const auto valueError = ValidateFw2051ScpProfileValue(
                definition.registerOffset, *value);
            if (!valueError.empty())
            {
                return "Quad " + std::to_string(quad.quad) + ": "
                    + valueError;
            }
            if (definition.dependencyRule ==
                Fw2051ScpDependencyRule::None)
            {
                continue;
            }
            const auto dependency = Fw2051ScpQuadRegisterValue(
                quad, definition.dependencyRegister);
            if (!dependency
                || !Fw2051ScpDependencySatisfied(
                    definition, *value, *dependency))
            {
                return "Quad " + std::to_string(quad.quad) + " "
                    + definition.name + " does not satisfy its "
                    + definition.dependencyName + " dependency.";
            }
        }
    }
    return {};
}

} // namespace

MvmeExportResult GenerateFw2051MvmeScript(
    const ScpProfile& profile,
    const std::string& generatedTimestamp)
{
    MvmeExportResult result;
    if (profile.formatVersion != Fw2051ScpProfileFormatVersion)
    {
        result.message = "The SCP profile version is unsupported for MVME export.";
        return result;
    }
    const auto validation = ValidateFw2051ScpConfiguration(
        profile.configuration);
    if (!validation.empty())
    {
        result.message = "The SCP profile cannot be exported: " + validation;
        return result;
    }
    const auto valueValidation = ValidateExportValues(profile.configuration);
    if (!valueValidation.empty())
    {
        result.message = "The SCP profile cannot be exported: "
            + valueValidation;
        return result;
    }
    if (generatedTimestamp.empty()
        || generatedTimestamp.find_first_of("\r\n") != std::string::npos)
    {
        result.message = "The MVME export timestamp must be one nonempty line.";
        return result;
    }

    const auto& configuration = profile.configuration;
    result.sourceProfileChecksum = ComputeFw2051ScpProfileChecksum(
        configuration);
    result.valueCount = Fw2051ScpConfigurationValueCount;

    std::ostringstream output;
    output << "# ===== BEGIN FIDGET MVME EXPORT =====\n"
           << "# FIDGET export\n"
           << "# source_profile_checksum: "
           << result.sourceProfileChecksum << '\n'
           << "# module_hardware_id: "
           << Hexadecimal16(configuration.hardwareId) << '\n'
           << "# module_firmware: "
           << Hexadecimal16(configuration.firmwareRevision) << '\n'
           << "# vme_base: "
           << Hexadecimal32(configuration.baseAddress) << '\n'
           << "# generated_utc: " << generatedTimestamp << '\n'
           << "# value_count: " << result.valueCount << "\n"
           << "#\n"
           << "# Global profile values (5)\n"
           << "# global 1/5 VME base: "
           << Hexadecimal32(configuration.baseAddress)
           << " (module address, not a register write)\n"
           << "# global 2/5 Hardware ID: "
           << Hexadecimal16(configuration.hardwareId)
           << " (identity check, not a register write)\n"
           << "# global 3/5 Firmware revision: "
           << Hexadecimal16(configuration.firmwareRevision)
           << " (identity check, not a register write)\n"
           << "# global 4/5 IRQ level\n"
           << "0x6010 " << configuration.irqLevel << '\n'
           << "# global 5/5 Output format\n"
           << "0x6044 " << configuration.outputFormat << "\n\n";

    for (const auto& quad : configuration.quads)
    {
        output << "# Quad " << quad.quad << '\n'
               << "0x6100 " << quad.quad << '\n';
        for (const auto& definition : Fw2051ScpSettingRegistry)
        {
            const auto value = Fw2051ScpQuadRegisterValue(
                quad, definition.registerOffset);
            if (!value)
            {
                result.message =
                    "The FW2051 registry changed during MVME export.";
                result.text.clear();
                result.sourceProfileChecksum = 0U;
                result.valueCount = 0U;
                return result;
            }
            output << Hexadecimal16(definition.registerOffset)
                   << ' ' << *value << " # " << definition.name << '\n';
        }
        output << '\n';
    }

    output << "# Park the FW2051 bank selector at quad 0\n"
           << "0x6100 0\n"
           << "# ===== END FIDGET MVME EXPORT =====\n";
    if (!output)
    {
        result.message = "Could not format the MVME settings export.";
        result.sourceProfileChecksum = 0U;
        result.valueCount = 0U;
        return result;
    }

    result.success = true;
    result.text = output.str();
    result.message = "Generated an MVME settings block for all 141 profile values.";
    return result;
}

} // namespace fidget
