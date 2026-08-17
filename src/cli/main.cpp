#include "cli/CliApp.h"

#include "hardware/MvlcCommandTransport.h"
#include "hardware/MvlcDataReceiver.h"
#include "hardware/OwnershipService.h"

#include <cerrno>
#include <csignal>
#include <iostream>
#include <memory>
#include <poll.h>
#include <unistd.h>

namespace {

volatile std::sig_atomic_t InterruptRequested = 0;

void HandleInterrupt(int)
{
    InterruptRequested = 1;
}

fidget::CliSessionWaitResult WaitForSessionInput()
{
    pollfd input{};
    input.fd = STDIN_FILENO;
    input.events = POLLIN | POLLHUP;
    const int result = ::poll(&input, 1U, 1000);
    if (result == 0)
    {
        return fidget::CliSessionWaitResult::OneSecondElapsed;
    }
    if (result < 0)
    {
        return errno == EINTR
            ? fidget::CliSessionWaitResult::Interrupted
            : fidget::CliSessionWaitResult::Error;
    }
    if ((input.revents & (POLLIN | POLLHUP)) != 0)
    {
        return fidget::CliSessionWaitResult::InputReady;
    }
    return fidget::CliSessionWaitResult::Error;
}

} // namespace

int main(int argc, char** argv)
{
    const auto parsed = fidget::ParseCliOptions(argc, argv);
    if (!parsed.success)
    {
        std::cerr << "error: " << parsed.error << '\n'
                  << fidget::FidgetCliUsage();
        return 1;
    }
    if (parsed.options.showHelp)
    {
        std::cout << fidget::FidgetCliUsage();
        return 0;
    }

    struct sigaction interruptAction{};
    interruptAction.sa_handler = HandleInterrupt;
    sigemptyset(&interruptAction.sa_mask);
    interruptAction.sa_flags = 0;
    (void)sigaction(SIGINT, &interruptAction, nullptr);
    auto transport = std::make_unique<fidget::MvlcCommandTransport>();
    auto dataReceiver = std::make_unique<fidget::MvlcDataReceiver>();
    fidget::OwnershipService tunerControl(
        std::move(transport),
        std::move(dataReceiver));
    const auto interrupted = [] { return InterruptRequested != 0; };
    if (parsed.options.command == fidget::CliCommand::Status)
    {
        return fidget::RunCliStatus(
            parsed.options,
            tunerControl,
            std::cout,
            std::cerr,
            interrupted);
    }
    if (parsed.options.command == fidget::CliCommand::Session)
    {
        return fidget::RunCliSession(
            parsed.options,
            tunerControl,
            std::cin,
            std::cout,
            std::cerr,
            interrupted,
            WaitForSessionInput);
    }
    if (parsed.options.command == fidget::CliCommand::Audit)
    {
        return fidget::RunCliAudit(
            parsed.options,
            tunerControl,
            std::cin,
            std::cout,
            std::cerr,
            interrupted);
    }
    if (parsed.options.command == fidget::CliCommand::Capture)
    {
        return fidget::RunCliCapture(
            parsed.options,
            tunerControl,
            std::cin,
            std::cout,
            std::cerr,
            interrupted);
    }
    if (parsed.options.command == fidget::CliCommand::Compare)
    {
        return fidget::RunCliCompare(
            parsed.options,
            tunerControl,
            std::cin,
            std::cout,
            std::cerr,
            interrupted);
    }
    if (parsed.options.command == fidget::CliCommand::Startup)
    {
        return fidget::RunCliStartup(
            parsed.options,
            tunerControl,
            std::cin,
            std::cout,
            std::cerr,
            interrupted);
    }
    if (parsed.options.command == fidget::CliCommand::Acquire)
    {
        return fidget::RunCliAcquire(
            parsed.options,
            tunerControl,
            std::cin,
            std::cout,
            std::cerr,
            interrupted,
            WaitForSessionInput);
    }
    return fidget::RunCliApply(
        parsed.options,
        tunerControl,
        std::cin,
        std::cout,
        std::cerr,
        interrupted);
}
