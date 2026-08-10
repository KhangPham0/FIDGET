#ifndef FIDGET_CORE_STARTUP_AUDIT_H
#define FIDGET_CORE_STARTUP_AUDIT_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fidget {

inline constexpr std::size_t Fw2051StartupAuditRegisterCount = 37U;

enum class StartupAuditState
{
    NotRun,
    Reading,
    Complete,
    Failed,
};

enum class StartupAuditRole
{
    Required,
    ReadoutContract,
    FormatDependent,
    ExperimentDependent,
    Informational,
};

enum class StartupAuditAssessment
{
    Ready,
    Blocked,
    Warning,
    Inherited,
    NotApplicable,
};

struct Fw2051StartupAuditRegisterDefinition
{
    std::uint16_t registerOffset;
    const char* name;
    const char* group;
    StartupAuditRole role;
};

struct StartupAuditRow
{
    std::uint16_t registerOffset = 0U;
    std::string name;
    std::string group;
    std::uint16_t value = 0U;
    StartupAuditRole role = StartupAuditRole::Informational;
    StartupAuditAssessment assessment = StartupAuditAssessment::Inherited;
    std::string note;
};

struct StartupAuditResult
{
    StartupAuditState state = StartupAuditState::NotRun;
    std::string message = "No module-wide startup audit has been run";
    std::uint32_t baseAddress = 0U;
    std::uint16_t hardwareId = 0U;
    std::uint16_t firmwareRevision = 0U;
    std::size_t registersRead = 0U;
    std::size_t requiredChecks = 0U;
    std::size_t requiredReady = 0U;
    std::size_t blockingIssues = 0U;
    std::size_t warnings = 0U;
    bool readyForDiagnosticStart = false;
    bool vmeWritesIssued = false;
    std::vector<StartupAuditRow> rows;
};

extern const std::array<
    Fw2051StartupAuditRegisterDefinition,
    Fw2051StartupAuditRegisterCount> Fw2051StartupAuditRegisterTable;

[[nodiscard]] StartupAuditResult ClassifyFw2051StartupAudit(
    std::uint32_t baseAddress,
    const std::array<
        std::uint16_t,
        Fw2051StartupAuditRegisterCount>& values);

[[nodiscard]] const char* StartupAuditRoleName(
    StartupAuditRole role) noexcept;

[[nodiscard]] const char* StartupAuditAssessmentName(
    StartupAuditAssessment assessment) noexcept;

} // namespace fidget

#endif
