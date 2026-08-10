#include "core/StartupAudit.h"

#include <algorithm>
#include <utility>

namespace fidget {

const std::array<
    Fw2051StartupAuditRegisterDefinition,
    Fw2051StartupAuditRegisterCount> Fw2051StartupAuditRegisterTable{{
    {0x6004U, "Module ID", "Identity", StartupAuditRole::ReadoutContract},
    {0x6006U, "Fast MBLT", "Transport", StartupAuditRole::ReadoutContract},
    {0x6008U, "Hardware ID", "Identity", StartupAuditRole::Required},
    {0x600EU, "Firmware revision", "Identity", StartupAuditRole::Required},
    {0x6010U, "IRQ level", "IRQ/readout", StartupAuditRole::Required},
    {0x6012U, "IRQ vector", "IRQ/readout", StartupAuditRole::Informational},
    {0x6018U, "IRQ data threshold", "IRQ/readout", StartupAuditRole::Required},
    {0x601AU, "Maximum transfer data", "IRQ/readout",
     StartupAuditRole::ReadoutContract},
    {0x601CU, "IRQ source", "IRQ/readout", StartupAuditRole::Required},
    {0x601EU, "IRQ event threshold", "IRQ/readout", StartupAuditRole::Required},
    {0x6032U, "Data-length format", "FIFO/readout",
     StartupAuditRole::Informational},
    {0x6036U, "Multi-event mode", "FIFO/readout", StartupAuditRole::Required},
    {0x6038U, "Marking type", "Event format",
     StartupAuditRole::ReadoutContract},
    {0x603AU, "Acquisition enabled", "Run state",
     StartupAuditRole::ReadoutContract},
    {0x6042U, "TDC resolution", "Event format",
     StartupAuditRole::ReadoutContract},
    {0x6044U, "Output format", "Event format", StartupAuditRole::Required},
    {0x6046U, "ADC resolution", "Event format",
     StartupAuditRole::Informational},
    {0x6050U, "Window start", "WOI trigger", StartupAuditRole::FormatDependent},
    {0x6054U, "Window width", "WOI trigger", StartupAuditRole::FormatDependent},
    {0x6058U, "Trigger source", "WOI trigger", StartupAuditRole::FormatDependent},
    {0x605CU, "First-hit mode", "WOI trigger", StartupAuditRole::FormatDependent},
    {0x605EU, "Trigger output source", "Trigger/IO",
     StartupAuditRole::ExperimentDependent},
    {0x6060U, "ECL3 input", "Trigger/IO", StartupAuditRole::ExperimentDependent},
    {0x6062U, "ECL2 input", "Trigger/IO", StartupAuditRole::ExperimentDependent},
    {0x6064U, "ECL1 input", "Trigger/IO", StartupAuditRole::ExperimentDependent},
    {0x6066U, "ECL0 output", "Trigger/IO", StartupAuditRole::ExperimentDependent},
    {0x6068U, "NIM4 input", "Trigger/IO", StartupAuditRole::ExperimentDependent},
    {0x606AU, "NIM3 input", "Trigger/IO", StartupAuditRole::ExperimentDependent},
    {0x606CU, "NIM2 input", "Trigger/IO", StartupAuditRole::ExperimentDependent},
    {0x6070U, "Test pulser", "Diagnostics", StartupAuditRole::ExperimentDependent},
    {0x6072U, "Test-pulser amplitude", "Diagnostics",
     StartupAuditRole::ExperimentDependent},
    {0x6074U, "NIM0 output", "Trigger/IO", StartupAuditRole::ExperimentDependent},
    {0x607AU, "Monitor enabled", "Diagnostics",
     StartupAuditRole::ExperimentDependent},
    {0x607CU, "Monitor channel", "Diagnostics",
     StartupAuditRole::ExperimentDependent},
    {0x607EU, "Monitor waveform", "Diagnostics",
     StartupAuditRole::ExperimentDependent},
    {0x6096U, "Timestamp sources", "Timestamp",
     StartupAuditRole::ExperimentDependent},
    {0x6098U, "Timestamp divisor", "Timestamp",
     StartupAuditRole::ExperimentDependent},
}};

StartupAuditResult ClassifyFw2051StartupAudit(
    const std::uint32_t baseAddress,
    const std::array<
        std::uint16_t,
        Fw2051StartupAuditRegisterCount>& values)
{
    StartupAuditResult result;
    result.state = StartupAuditState::Complete;
    result.baseAddress = baseAddress;
    result.vmeWritesIssued = false;
    result.rows.reserve(Fw2051StartupAuditRegisterCount);

    for (std::size_t index = 0U;
         index < Fw2051StartupAuditRegisterTable.size(); ++index)
    {
        const auto& definition = Fw2051StartupAuditRegisterTable[index];
        result.rows.push_back(StartupAuditRow{
            definition.registerOffset,
            definition.name,
            definition.group,
            values[index],
            definition.role,
            StartupAuditAssessment::Inherited,
            {}});
    }

    const auto findRow = [&result](const std::uint16_t offset)
        -> StartupAuditRow& {
        const auto found = std::find_if(
            result.rows.begin(), result.rows.end(),
            [offset](const StartupAuditRow& row) {
                return row.registerOffset == offset;
            });
        return *found;
    };

    const auto set = [&findRow](
                         const std::uint16_t offset,
                         const StartupAuditAssessment assessment,
                         std::string note) {
        auto& row = findRow(offset);
        row.assessment = assessment;
        row.note = std::move(note);
    };

    const auto value = [&findRow](const std::uint16_t offset) {
        return findRow(offset).value;
    };

    set(0x6004U, StartupAuditAssessment::Inherited,
        "Copied into each event header; any configured module ID is accepted.");
    set(0x6006U,
        value(0x6006U) <= 1U ? StartupAuditAssessment::Inherited
                             : StartupAuditAssessment::Warning,
        value(0x6006U) == 0U
            ? "MBLT remains valid but uses the slower timing option."
            : value(0x6006U) == 1U
                ? "Fast MBLT timing is enabled."
                : "Documented values are 0 and 1.");

    const bool supportedHardware =
        value(0x6008U) == 0x5007U || value(0x6008U) == 0x500CU;
    set(0x6008U,
        supportedHardware ? StartupAuditAssessment::Ready
                          : StartupAuditAssessment::Blocked,
        supportedHardware
            ? "Supported MDPP-32 hardware."
            : "The selected base does not identify as a supported MDPP-32.");

    const bool scpFirmware = ((value(0x600EU) >> 12U) & 0x0FU) == 2U;
    set(0x600EU,
        scpFirmware ? StartupAuditAssessment::Ready
                    : StartupAuditAssessment::Blocked,
        scpFirmware
            ? "SCP register map detected."
            : "This audit and waveform tuner require SCP firmware.");

    const bool irqLevelReady =
        value(0x6010U) >= 1U && value(0x6010U) <= 6U;
    set(0x6010U,
        irqLevelReady ? StartupAuditAssessment::Ready
                      : StartupAuditAssessment::Blocked,
        irqLevelReady
            ? "Accepted by the tuner's IRQ1-through-IRQ6 stack trigger."
            : "Direct acquisition requires an IRQ level from 1 through 6.");
    set(0x6012U, StartupAuditAssessment::NotApplicable,
        "The tuner uses the MVLC no-IACK IRQ trigger, so the vector is not consumed.");

    const std::uint16_t irqSource = value(0x601CU);
    const bool irqSourceReady = irqSource <= 1U;
    set(0x601CU,
        irqSourceReady ? StartupAuditAssessment::Ready
                       : StartupAuditAssessment::Blocked,
        irqSource == 0U
            ? "IRQ is driven by the event threshold."
            : irqSource == 1U
                ? "IRQ is driven by the data-word threshold."
                : "Documented IRQ-source values are 0 and 1.");
    set(0x6018U,
        irqSource == 1U
            ? (value(0x6018U) <= 0x7FFFU
                   ? StartupAuditAssessment::Ready
                   : StartupAuditAssessment::Blocked)
            : StartupAuditAssessment::NotApplicable,
        irqSource == 1U
            ? "Active IRQ threshold; valid range is 0 through 32767 words."
            : "Inactive because IRQ source is not the data threshold.");
    set(0x601EU,
        irqSource == 0U
            ? (value(0x601EU) <= 0x7FFFU
                   ? StartupAuditAssessment::Ready
                   : StartupAuditAssessment::Blocked)
            : StartupAuditAssessment::NotApplicable,
        irqSource == 0U
            ? "Active IRQ threshold; valid range is 0 through 32767 events."
            : "Inactive because IRQ source is not the event threshold.");

    const std::uint16_t multiEvent = value(0x6036U);
    const std::uint16_t multiEventMode = multiEvent & 0x03U;
    const bool multiEventEncodingValid =
        (multiEvent & 0xFFF0U) == 0U && multiEventMode != 2U;
    const bool eobInsteadOfBerr = (multiEvent & 0x04U) != 0U;
    set(0x6036U,
        !multiEventEncodingValid
            ? StartupAuditAssessment::Blocked
            : eobInsteadOfBerr ? StartupAuditAssessment::Warning
                               : StartupAuditAssessment::Ready,
        !multiEventEncodingValid
            ? "The low mode bits must be 0, 1, or 3 and only bits 3:0 are documented."
            : eobInsteadOfBerr
                ? "EOB replaces BERR; the current diagnostic stack is proven with BERR termination."
                : "Compatible with the tuner's bounded MBLT/BERR readout stack.");

    set(0x601AU,
        multiEventMode == 3U || (multiEvent & 0x08U) != 0U
            ? StartupAuditAssessment::Inherited
            : StartupAuditAssessment::NotApplicable,
        multiEventMode == 3U || (multiEvent & 0x08U) != 0U
            ? "Controls the transfer boundary for the selected multi-event mode."
            : "Not used by the selected multi-event mode.");
    set(0x6032U,
        value(0x6032U) <= 4U ? StartupAuditAssessment::NotApplicable
                             : StartupAuditAssessment::Warning,
        value(0x6032U) <= 4U
            ? "The tuner does not poll FIFO length register 0x6030."
            : "The value is outside the documented data-length encodings.");

    const std::uint16_t markingType = value(0x6038U);
    const bool markingValid =
        markingType == 0U || markingType == 1U || markingType == 3U;
    set(0x6038U,
        markingValid ? StartupAuditAssessment::Inherited
                     : StartupAuditAssessment::Warning,
        markingValid
            ? "Preserved; waveform decoding accepts the event marker as timestamp metadata."
            : "Documented marking types are event counter (0), timestamp (1), and extended timestamp (3).");
    set(0x603AU,
        value(0x603AU) == 0U ? StartupAuditAssessment::Inherited
                             : StartupAuditAssessment::Warning,
        value(0x603AU) == 0U
            ? "Module is stopped, as expected before tuner startup."
            : "Module still accepts triggers; the direct-start sequence will stop and reset it first.");
    set(0x6042U,
        value(0x6042U) <= 5U ? StartupAuditAssessment::Inherited
                             : StartupAuditAssessment::Warning,
        value(0x6042U) <= 5U
            ? "Valid documented fine-time resolution."
            : "Documented TDC-resolution values are 0 through 5.");

    const std::uint16_t outputFormat = value(0x6044U);
    const bool sampledOutput = outputFormat == 16U || outputFormat == 24U;
    set(0x6044U,
        sampledOutput ? StartupAuditAssessment::Ready
                      : StartupAuditAssessment::Blocked,
        outputFormat == 16U
            ? "Window-of-interest sample output is enabled."
            : outputFormat == 24U
                ? "Standard-streaming sample output is enabled."
                : "Direct waveform acquisition requires output format 16 or 24.");
    set(0x6046U, StartupAuditAssessment::Inherited,
        "Read-only amplitude resolution metadata.");

    const bool windowMode = outputFormat == 16U;
    for (const std::uint16_t offset :
         {std::uint16_t{0x6050U}, std::uint16_t{0x6054U},
          std::uint16_t{0x6058U}, std::uint16_t{0x605CU}})
    {
        set(offset,
            windowMode ? StartupAuditAssessment::Inherited
                       : StartupAuditAssessment::NotApplicable,
            windowMode
                ? "Used by sampled window-of-interest mode."
                : "Ignored by standard streaming, where channels are independent.");
    }

    for (const std::uint16_t offset :
         {std::uint16_t{0x605EU}, std::uint16_t{0x6060U},
          std::uint16_t{0x6062U}, std::uint16_t{0x6064U},
          std::uint16_t{0x6066U}, std::uint16_t{0x6068U},
          std::uint16_t{0x606AU}, std::uint16_t{0x606CU},
          std::uint16_t{0x6074U}})
    {
        set(offset, StartupAuditAssessment::Inherited,
            "Experiment wiring choice; recorded for later standalone initialization.");
    }

    const bool pulserEnabled = value(0x6070U) != 0U;
    set(0x6070U,
        pulserEnabled ? StartupAuditAssessment::Warning
                      : StartupAuditAssessment::Inherited,
        pulserEnabled
            ? "The internal pulser is enabled and can degrade real input signals."
            : "Internal pulser is off.");
    set(0x6072U,
        pulserEnabled ? StartupAuditAssessment::Inherited
                      : StartupAuditAssessment::NotApplicable,
        pulserEnabled
            ? "Active internal-pulser amplitude."
            : "Pulser amplitude is inactive while the pulser is off.");

    const bool monitorEnabled = value(0x607AU) != 0U;
    set(0x607AU,
        monitorEnabled ? StartupAuditAssessment::Warning
                       : StartupAuditAssessment::Inherited,
        monitorEnabled
            ? "Analog monitor output is enabled; this is an explicit diagnostic choice."
            : "Analog monitor output is off.");
    for (const std::uint16_t offset :
         {std::uint16_t{0x607CU}, std::uint16_t{0x607EU}})
    {
        set(offset,
            monitorEnabled ? StartupAuditAssessment::Inherited
                           : StartupAuditAssessment::NotApplicable,
            monitorEnabled
                ? "Active analog-monitor selection."
                : "Inactive while analog monitoring is off.");
    }

    set(0x6096U,
        (value(0x6096U) & 0xFFFCU) == 0U
            ? StartupAuditAssessment::Inherited
            : StartupAuditAssessment::Warning,
        (value(0x6096U) & 0xFFFCU) == 0U
            ? "SCP timestamp clock/reset selection; must match experiment wiring."
            : "SCP documents only timestamp-source bits 1:0.");
    set(0x6098U, StartupAuditAssessment::Inherited,
        "Timestamp divisor; zero represents division by 65536.");

    result.hardwareId = value(0x6008U);
    result.firmwareRevision = value(0x600EU);
    result.registersRead = result.rows.size();

    for (const auto& row : result.rows)
    {
        if (row.role == StartupAuditRole::Required &&
            row.assessment != StartupAuditAssessment::NotApplicable)
        {
            ++result.requiredChecks;
            if (row.assessment == StartupAuditAssessment::Ready ||
                row.assessment == StartupAuditAssessment::Warning)
            {
                ++result.requiredReady;
            }
        }
        if (row.assessment == StartupAuditAssessment::Blocked)
        {
            ++result.blockingIssues;
        }
        else if (row.assessment == StartupAuditAssessment::Warning)
        {
            ++result.warnings;
        }
    }

    result.readyForDiagnosticStart = result.blockingIssues == 0U;
    result.message =
        "Read " + std::to_string(result.registersRead) +
        " module-wide registers with zero VME writes. Required checks " +
        std::to_string(result.requiredReady) + "/" +
        std::to_string(result.requiredChecks) + "; " +
        std::to_string(result.blockingIssues) + " blocking issue(s), " +
        std::to_string(result.warnings) + " warning(s).";
    return result;
}

const char* StartupAuditRoleName(const StartupAuditRole role) noexcept
{
    switch (role)
    {
    case StartupAuditRole::Required:
        return "Required";
    case StartupAuditRole::ReadoutContract:
        return "Readout";
    case StartupAuditRole::FormatDependent:
        return "Format";
    case StartupAuditRole::ExperimentDependent:
        return "Experiment";
    case StartupAuditRole::Informational:
        return "Info";
    }
    return "Unknown";
}

const char* StartupAuditAssessmentName(
    const StartupAuditAssessment assessment) noexcept
{
    switch (assessment)
    {
    case StartupAuditAssessment::Ready:
        return "Ready";
    case StartupAuditAssessment::Blocked:
        return "Blocked";
    case StartupAuditAssessment::Warning:
        return "Warning";
    case StartupAuditAssessment::Inherited:
        return "Inherited";
    case StartupAuditAssessment::NotApplicable:
        return "N/A";
    }
    return "Unknown";
}

} // namespace fidget
