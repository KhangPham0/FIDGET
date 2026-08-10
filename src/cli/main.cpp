#include "cli/CliApp.h"

#include "hardware/MvlcCommandTransport.h"
#include "hardware/OwnershipService.h"

#include <csignal>
#include <iostream>
#include <memory>

namespace {

volatile std::sig_atomic_t InterruptRequested = 0;

void HandleInterrupt(int)
{
    InterruptRequested = 1;
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

    std::signal(SIGINT, HandleInterrupt);
    auto transport = std::make_unique<fidget::MvlcCommandTransport>();
    fidget::OwnershipService tunerControl(std::move(transport));
    return fidget::RunCliStatus(
        parsed.options,
        tunerControl,
        std::cout,
        std::cerr,
        [] { return InterruptRequested != 0; });
}
