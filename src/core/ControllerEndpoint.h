#ifndef FIDGET_CORE_CONTROLLER_ENDPOINT_H
#define FIDGET_CORE_CONTROLLER_ENDPOINT_H

#include <cstdint>
#include <string>

namespace fidget {

enum class ControllerEndpointKind
{
    DirectEthernet,
    SshBridge,
};

// Project-independent routing fields for controller transport validation.
// Authentication remains external to FIDGET, so this request deliberately
// has no password, key, or secret field. Endpoint names stay opaque here;
// the socket transport resolves hostnames and addresses to IPv4 today.
struct ControllerEndpointRequest
{
    ControllerEndpointKind kind = ControllerEndpointKind::DirectEthernet;
    std::string mvlcHost;
    std::uint16_t mvlcCommandPort = 32768U;
    std::string sshDestination;
    std::string remoteBridgeCommand = "fidget_bridge";
};

enum class ControllerEndpointValidationIssue
{
    None,
    UnknownKind,
    InvalidMvlcHost,
    InvalidMvlcCommandPort,
    InvalidSshDestination,
    InvalidRemoteBridgeCommand,
};

struct ControllerEndpointValidationResult
{
    bool success = false;
    ControllerEndpointValidationIssue issue =
        ControllerEndpointValidationIssue::UnknownKind;
    std::string message;
};

[[nodiscard]] ControllerEndpointValidationResult
ValidateControllerEndpoint(const ControllerEndpointRequest& request);

} // namespace fidget

#endif
