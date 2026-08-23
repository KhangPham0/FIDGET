#ifndef FIDGET_CORE_TUNER_CONTROL_H
#define FIDGET_CORE_TUNER_CONTROL_H

#include "core/CrateProject.h"
#include "core/TunerSnapshot.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <variant>

namespace fidget {

struct EditTunerTargetCommand
{
    TunerTargetInput input;
};

struct SelectTunerTargetCommand
{
};

struct ProbeTunerTargetCommand
{
};

struct OpenTunerTargetSessionCommand
{
};

struct ClearTunerTargetCommand
{
};

struct UseCrateProjectCommand
{
    std::string projectPath;
    CrateProject project;
    std::size_t activeModuleIndex = 0U;
};

struct ClearCrateProjectCommand
{
};

struct CheckStatusCommand
{
};

struct SetMvmeHandoffConfirmedCommand
{
    bool confirmed = false;
};

struct OpenSessionCommand
{
};

struct ReleaseSessionCommand
{
};

struct RunStartupAuditCommand
{
};

struct CaptureConfigurationCommand
{
};

struct SaveProfileCommand
{
    std::string path;
};

struct LoadProfileCommand
{
    std::string path;
};

struct ExportMvmeScriptCommand
{
    std::string path;
    bool allowOverwrite = false;
};

struct ApplyProfileRowCommand
{
    std::uint16_t registerOffset = 0U;
    std::uint16_t quad = 0U;
};

struct ApplyAllDifferencesCommand
{
};

struct RunDeterministicStartupCommand
{
    bool confirmed = false;
};

struct StartDiagnosticAcquisitionCommand
{
    std::uint16_t channel = 0U;
};

struct StopDiagnosticAcquisitionCommand
{
};

struct ChangeDiagnosticSourceCommand
{
    std::uint8_t source = 0U;
};

struct ApplyDiagnosticPreviewCommand
{
    std::uint16_t registerOffset = 0U;
    std::uint16_t value = 0U;
};

struct RestoreDiagnosticPreviewCommand
{
};

struct RecoverDiagnosticOrphanCommand
{
    bool confirmed = false;
};

using TunerCommand = std::variant<
    EditTunerTargetCommand,
    SelectTunerTargetCommand,
    ProbeTunerTargetCommand,
    OpenTunerTargetSessionCommand,
    ClearTunerTargetCommand,
    UseCrateProjectCommand,
    ClearCrateProjectCommand,
    CheckStatusCommand,
    SetMvmeHandoffConfirmedCommand,
    OpenSessionCommand,
    RunStartupAuditCommand,
    CaptureConfigurationCommand,
    SaveProfileCommand,
    LoadProfileCommand,
    ExportMvmeScriptCommand,
    ApplyProfileRowCommand,
    ApplyAllDifferencesCommand,
    RunDeterministicStartupCommand,
    StartDiagnosticAcquisitionCommand,
    StopDiagnosticAcquisitionCommand,
    ChangeDiagnosticSourceCommand,
    ApplyDiagnosticPreviewCommand,
    RestoreDiagnosticPreviewCommand,
    RecoverDiagnosticOrphanCommand,
    ReleaseSessionCommand>;

class ITunerControl
{
public:
    virtual ~ITunerControl();

    [[nodiscard]] virtual std::shared_ptr<const TunerSnapshot>
        CurrentSnapshot() const = 0;
    virtual void Submit(TunerCommand command) = 0;
};

} // namespace fidget

#endif
