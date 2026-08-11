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

using TunerCommand = std::variant<
    UseCrateProjectCommand,
    ClearCrateProjectCommand,
    CheckStatusCommand,
    SetMvmeHandoffConfirmedCommand,
    OpenSessionCommand,
    RunStartupAuditCommand,
    CaptureConfigurationCommand,
    SaveProfileCommand,
    LoadProfileCommand,
    ApplyProfileRowCommand,
    ApplyAllDifferencesCommand,
    RunDeterministicStartupCommand,
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
