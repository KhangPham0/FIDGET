#ifndef FIDGET_CORE_TUNER_CONTROL_H
#define FIDGET_CORE_TUNER_CONTROL_H

#include "core/CrateProject.h"
#include "core/TunerSnapshot.h"

#include <cstddef>
#include <memory>
#include <string>
#include <variant>

namespace fidget {

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

using TunerCommand = std::variant<
    UseCrateProjectCommand,
    ClearCrateProjectCommand,
    CheckStatusCommand,
    SetMvmeHandoffConfirmedCommand,
    OpenSessionCommand,
    RunStartupAuditCommand,
    CaptureConfigurationCommand,
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
