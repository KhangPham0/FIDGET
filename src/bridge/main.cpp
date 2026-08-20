#include "hardware/BridgeRelay.h"

#include <charconv>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <string>
#include <system_error>
#include <unistd.h>

namespace {

bool ParseCommandPort(const std::string& text, std::uint16_t& port)
{
    std::uint32_t parsed = 0U;
    const auto conversion = std::from_chars(
        text.data(), text.data() + text.size(), parsed, 10);
    if (conversion.ec != std::errc{}
        || conversion.ptr != text.data() + text.size()
        || parsed == 0U
        || parsed > 0xFFFFU)
    {
        return false;
    }
    port = static_cast<std::uint16_t>(parsed);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        std::cerr << "usage: fidget_bridge <mvlc-host> <command-port>\n";
        return 1;
    }

    std::uint16_t commandPort = 0U;
    if (!ParseCommandPort(argv[2], commandPort))
    {
        std::cerr << "fidget_bridge: invalid command port\n";
        return 1;
    }
    std::signal(SIGPIPE, SIG_IGN);
    const auto result = fidget::RunBridgeRelay(
        argv[1], commandPort, STDIN_FILENO, STDOUT_FILENO);
    if (!result.success)
    {
        std::cerr << "fidget_bridge: " << result.error << '\n';
        return 1;
    }
    return 0;
}
