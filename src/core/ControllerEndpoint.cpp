#include "core/ControllerEndpoint.h"

#include <algorithm>
#include <cstddef>
#include <utility>

namespace fidget {
namespace {

constexpr std::size_t MaximumEndpointHostLength = 255U;
constexpr std::size_t MaximumSshDestinationLength = 255U;
constexpr std::size_t MaximumRemoteBridgeCommandLength = 511U;

bool IsSingleOpaqueArgument(
    const std::string& value,
    const std::size_t maximumLength)
{
    if (value.empty() || value.size() > maximumLength)
        return false;

    return std::all_of(
        value.begin(), value.end(),
        [](const unsigned char character) {
            return character > 0x20U && character != 0x7FU;
        });
}

ControllerEndpointValidationResult Failure(
    const ControllerEndpointValidationIssue issue,
    std::string message)
{
    return {false, issue, std::move(message)};
}

} // namespace

ControllerEndpointValidationResult ValidateControllerEndpoint(
    const ControllerEndpointRequest& request)
{
    if (request.kind != ControllerEndpointKind::DirectEthernet
        && request.kind != ControllerEndpointKind::SshBridge)
    {
        return Failure(
            ControllerEndpointValidationIssue::UnknownKind,
            "Select direct Ethernet or an SSH bridge.");
    }
    if (!IsSingleOpaqueArgument(
            request.mvlcHost, MaximumEndpointHostLength))
    {
        return Failure(
            ControllerEndpointValidationIssue::InvalidMvlcHost,
            "Enter an MVLC hostname or IPv4 address without whitespace.");
    }
    if (request.mvlcCommandPort == 0U
        || request.mvlcCommandPort == 0xFFFFU)
    {
        return Failure(
            ControllerEndpointValidationIssue::InvalidMvlcCommandPort,
            "Enter an MVLC command port whose following data port is valid.");
    }
    if (request.kind == ControllerEndpointKind::SshBridge)
    {
        if (!IsSingleOpaqueArgument(
                request.sshDestination, MaximumSshDestinationLength))
        {
            return Failure(
                ControllerEndpointValidationIssue::InvalidSshDestination,
                "Enter one SSH destination without whitespace.");
        }
        if (!IsSingleOpaqueArgument(
                request.remoteBridgeCommand,
                MaximumRemoteBridgeCommandLength))
        {
            return Failure(
                ControllerEndpointValidationIssue::InvalidRemoteBridgeCommand,
                "Enter one remote bridge command path without whitespace.");
        }
    }

    return {true, ControllerEndpointValidationIssue::None, {}};
}

} // namespace fidget
