#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/StartupAudit.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace {

using AuditValues = std::array<
    std::uint16_t,
    fidget::Fw2051StartupAuditRegisterCount>;

std::size_t RegisterIndex(const std::uint16_t registerOffset)
{
    const auto& table = fidget::Fw2051StartupAuditRegisterTable;
    const auto found = std::find_if(
        table.begin(), table.end(),
        [registerOffset](const auto& definition) {
            return definition.registerOffset == registerOffset;
        });
    REQUIRE(found != table.end());
    return static_cast<std::size_t>(found - table.begin());
}

AuditValues MakeReadyValues()
{
    AuditValues values{};
    const auto set = [&values](
                         const std::uint16_t registerOffset,
                         const std::uint16_t value) {
        values[RegisterIndex(registerOffset)] = value;
    };

    set(0x6004U, 0x0011U);
    set(0x6006U, 1U);
    set(0x6008U, 0x5007U);
    set(0x600EU, 0x2051U);
    set(0x6010U, 1U);
    set(0x6012U, 0U);
    set(0x6018U, 1U);
    set(0x601AU, 1U);
    set(0x601CU, 1U);
    set(0x601EU, 1U);
    set(0x6032U, 2U);
    set(0x6036U, 3U);
    set(0x6038U, 3U);
    set(0x603AU, 0U);
    set(0x6042U, 5U);
    set(0x6044U, 0x18U);
    set(0x6046U, 0U);
    set(0x6050U, 0x3FF0U);
    set(0x6054U, 32U);
    set(0x6058U, 0x0100U);
    set(0x605CU, 1U);
    set(0x605EU, 0x0100U);
    set(0x6060U, 0U);
    set(0x6062U, 0U);
    set(0x6064U, 0U);
    set(0x6066U, 0U);
    set(0x6068U, 1U);
    set(0x606AU, 1U);
    set(0x606CU, 1U);
    set(0x6070U, 0U);
    set(0x6072U, 400U);
    set(0x6074U, 1U);
    set(0x607AU, 0U);
    set(0x607CU, 0U);
    set(0x607EU, 0U);
    set(0x6096U, 0U);
    set(0x6098U, 1U);
    return values;
}

const fidget::StartupAuditRow& FindRow(
    const fidget::StartupAuditResult& result,
    const std::uint16_t registerOffset)
{
    const auto found = std::find_if(
        result.rows.begin(), result.rows.end(),
        [registerOffset](const auto& row) {
            return row.registerOffset == registerOffset;
        });
    REQUIRE(found != result.rows.end());
    return *found;
}

} // namespace

TEST_CASE("the FW2051 startup table retains all 37 register definitions")
{
    using fidget::StartupAuditRole;

    struct ExpectedDefinition
    {
        std::uint16_t registerOffset;
        const char* name;
        const char* group;
        StartupAuditRole role;
    };

    const std::array<ExpectedDefinition, 37> expected{{
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

    REQUIRE(fidget::Fw2051StartupAuditRegisterTable.size() == expected.size());
    for (std::size_t index = 0U; index < expected.size(); ++index)
    {
        const auto& actual = fidget::Fw2051StartupAuditRegisterTable[index];
        CHECK(actual.registerOffset == expected[index].registerOffset);
        CHECK(std::string(actual.name) == expected[index].name);
        CHECK(std::string(actual.group) == expected[index].group);
        CHECK(actual.role == expected[index].role);
    }
}

TEST_CASE("a proven ready configuration retains its exact audit result")
{
    using namespace fidget;

    const auto result = ClassifyFw2051StartupAudit(
        0x11000000U, MakeReadyValues());

    CHECK(result.state == StartupAuditState::Complete);
    CHECK(result.baseAddress == 0x11000000U);
    CHECK(result.hardwareId == 0x5007U);
    CHECK(result.firmwareRevision == 0x2051U);
    CHECK(result.registersRead == 37U);
    CHECK(result.rows.size() == 37U);
    CHECK(result.requiredChecks == 7U);
    CHECK(result.requiredReady == 7U);
    CHECK(result.blockingIssues == 0U);
    CHECK(result.warnings == 0U);
    CHECK(result.readyForDiagnosticStart);
    CHECK_FALSE(result.vmeWritesIssued);
    CHECK(result.message ==
          "Read 37 module-wide registers with zero VME writes. Required "
          "checks 7/7; 0 blocking issue(s), 0 warning(s).");
    CHECK(FindRow(result, 0x6044U).assessment ==
          StartupAuditAssessment::Ready);
    CHECK(FindRow(result, 0x6050U).assessment ==
          StartupAuditAssessment::NotApplicable);
}

TEST_CASE("each required startup check can independently block readiness")
{
    using namespace fidget;

    struct BlockedCase
    {
        const char* name;
        std::uint16_t registerOffset;
        std::uint16_t value;
        std::size_t requiredChecks;
        std::size_t requiredReady;
    };

    const std::array<BlockedCase, 7> cases{{
        {"hardware", 0x6008U, 0x5008U, 7U, 6U},
        {"firmware", 0x600EU, 0x1051U, 7U, 6U},
        {"IRQ level", 0x6010U, 0U, 7U, 6U},
        {"IRQ source", 0x601CU, 2U, 6U, 5U},
        {"active threshold", 0x6018U, 0x8000U, 7U, 6U},
        {"multi-event encoding", 0x6036U, 2U, 7U, 6U},
        {"output format", 0x6044U, 8U, 7U, 6U},
    }};

    for (const auto& blockedCase : cases)
    {
        CAPTURE(blockedCase.name);
        auto values = MakeReadyValues();
        values[RegisterIndex(blockedCase.registerOffset)] = blockedCase.value;
        const auto result = ClassifyFw2051StartupAudit(0U, values);

        CHECK_FALSE(result.readyForDiagnosticStart);
        CHECK(result.blockingIssues == 1U);
        CHECK(result.requiredChecks == blockedCase.requiredChecks);
        CHECK(result.requiredReady == blockedCase.requiredReady);
        CHECK(FindRow(result, blockedCase.registerOffset).assessment ==
              StartupAuditAssessment::Blocked);
    }
}

TEST_CASE("the event-threshold branch is the active seventh required check")
{
    using namespace fidget;

    auto values = MakeReadyValues();
    values[RegisterIndex(0x601CU)] = 0U;
    values[RegisterIndex(0x601EU)] = 0x8000U;
    const auto result = ClassifyFw2051StartupAudit(0U, values);

    CHECK_FALSE(result.readyForDiagnosticStart);
    CHECK(result.requiredChecks == 7U);
    CHECK(result.requiredReady == 6U);
    CHECK(result.blockingIssues == 1U);
    CHECK(FindRow(result, 0x6018U).assessment ==
          StartupAuditAssessment::NotApplicable);
    CHECK(FindRow(result, 0x601EU).assessment ==
          StartupAuditAssessment::Blocked);
}

TEST_CASE("every startup warning rule remains non-blocking")
{
    using namespace fidget;

    struct WarningCase
    {
        std::uint16_t registerOffset;
        std::uint16_t value;
        const char* noteFragment;
    };

    const std::array<WarningCase, 9> cases{{
        {0x6006U, 2U, "Documented values are 0 and 1"},
        {0x6036U, 7U, "EOB replaces BERR"},
        {0x6032U, 6U, "outside the documented data-length"},
        {0x6038U, 2U, "Documented marking types"},
        {0x603AU, 1U, "still accepts triggers"},
        {0x6042U, 6U, "Documented TDC-resolution"},
        {0x6070U, 1U, "internal pulser is enabled"},
        {0x607AU, 1U, "Analog monitor output is enabled"},
        {0x6096U, 4U, "only timestamp-source bits 1:0"},
    }};

    for (const auto& warningCase : cases)
    {
        auto values = MakeReadyValues();
        values[RegisterIndex(warningCase.registerOffset)] = warningCase.value;
        const auto result = ClassifyFw2051StartupAudit(0U, values);
        const auto& row = FindRow(result, warningCase.registerOffset);

        CHECK(result.readyForDiagnosticStart);
        CHECK(result.blockingIssues == 0U);
        CHECK(result.warnings == 1U);
        CHECK(row.assessment == StartupAuditAssessment::Warning);
        CHECK(row.note.find(warningCase.noteFragment) != std::string::npos);
        CHECK_FALSE(result.vmeWritesIssued);
    }
}

TEST_CASE("the prototype blocked configuration retains its exact counts")
{
    using namespace fidget;

    auto values = MakeReadyValues();
    values[RegisterIndex(0x6044U)] = 0x08U;
    values[RegisterIndex(0x603AU)] = 1U;
    values[RegisterIndex(0x6070U)] = 1U;
    const auto result = ClassifyFw2051StartupAudit(0U, values);

    CHECK_FALSE(result.readyForDiagnosticStart);
    CHECK(result.blockingIssues == 1U);
    CHECK(result.requiredChecks == 7U);
    CHECK(result.requiredReady == 6U);
    CHECK(result.warnings == 2U);
}

TEST_CASE("audit labels retain the prototype presentation vocabulary")
{
    using namespace fidget;

    CHECK(std::string(StartupAuditRoleName(StartupAuditRole::Required)) ==
          "Required");
    CHECK(std::string(StartupAuditRoleName(
              StartupAuditRole::ReadoutContract)) == "Readout");
    CHECK(std::string(StartupAuditRoleName(
              StartupAuditRole::FormatDependent)) == "Format");
    CHECK(std::string(StartupAuditRoleName(
              StartupAuditRole::ExperimentDependent)) == "Experiment");
    CHECK(std::string(StartupAuditRoleName(
              StartupAuditRole::Informational)) == "Info");
    CHECK(std::string(StartupAuditAssessmentName(
              StartupAuditAssessment::NotApplicable)) == "N/A");
}
