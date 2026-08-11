#ifndef FIDGET_CLI_CLI_APP_H
#define FIDGET_CLI_CLI_APP_H

#include "core/TunerControl.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>

namespace fidget {

enum class CliCommand
{
    Status,
    Session,
    Audit,
    Capture,
    Compare,
    Apply,
    ApplyAll,
    Startup,
};

enum class CliSessionWaitResult
{
    OneSecondElapsed,
    InputReady,
    Interrupted,
    Error,
};

struct CliOptions
{
    CliCommand command = CliCommand::Status;
    std::string projectPath;
    std::string host;
    std::optional<std::uint16_t> port;
    std::size_t moduleIndex = 0U;
    std::string savePath;
    std::string profilePath;
    std::optional<std::uint16_t> registerOffset;
    std::optional<std::uint16_t> quad;
    bool showHelp = false;
};

struct CliOptionsParseResult
{
    bool success = false;
    CliOptions options;
    std::string error;
};

using CliInterruptRequested = std::function<bool()>;
using CliWaitForSessionInput = std::function<CliSessionWaitResult()>;

[[nodiscard]] const char* FidgetCliUsage() noexcept;
[[nodiscard]] CliOptionsParseResult ParseCliOptions(
    int argumentCount,
    const char* const* arguments);

[[nodiscard]] int RunCliStatus(
    const CliOptions& options,
    ITunerControl& tunerControl,
    std::ostream& output,
    std::ostream& errorOutput,
    const CliInterruptRequested& interruptRequested);

[[nodiscard]] int RunCliSession(
    const CliOptions& options,
    ITunerControl& tunerControl,
    std::istream& input,
    std::ostream& output,
    std::ostream& errorOutput,
    const CliInterruptRequested& interruptRequested,
    const CliWaitForSessionInput& waitForInput);

[[nodiscard]] int RunCliAudit(
    const CliOptions& options,
    ITunerControl& tunerControl,
    std::istream& input,
    std::ostream& output,
    std::ostream& errorOutput,
    const CliInterruptRequested& interruptRequested);

[[nodiscard]] int RunCliCapture(
    const CliOptions& options,
    ITunerControl& tunerControl,
    std::istream& input,
    std::ostream& output,
    std::ostream& errorOutput,
    const CliInterruptRequested& interruptRequested);

[[nodiscard]] int RunCliCompare(
    const CliOptions& options,
    ITunerControl& tunerControl,
    std::istream& input,
    std::ostream& output,
    std::ostream& errorOutput,
    const CliInterruptRequested& interruptRequested);

[[nodiscard]] int RunCliApply(
    const CliOptions& options,
    ITunerControl& tunerControl,
    std::istream& input,
    std::ostream& output,
    std::ostream& errorOutput,
    const CliInterruptRequested& interruptRequested);

[[nodiscard]] int RunCliStartup(
    const CliOptions& options,
    ITunerControl& tunerControl,
    std::istream& input,
    std::ostream& output,
    std::ostream& errorOutput,
    const CliInterruptRequested& interruptRequested);

} // namespace fidget

#endif
