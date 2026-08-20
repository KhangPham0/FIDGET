#ifndef FIDGET_HARDWARE_BRIDGE_RELAY_H
#define FIDGET_HARDWARE_BRIDGE_RELAY_H

#include <cstdint>
#include <string>

namespace fidget {

struct BridgeRelayResult
{
    bool success = false;
    std::string error;
};

// inputDescriptor and outputDescriptor remain owned by the caller. The relay
// owns the two UDP sockets it opens for the duration of this call.
[[nodiscard]] BridgeRelayResult RunBridgeRelay(
    const std::string& mvlcHost,
    std::uint16_t commandPort,
    int inputDescriptor,
    int outputDescriptor);

} // namespace fidget

#endif
