#include "hardware/TransportFactory.h"

#include "core/ControllerEndpoint.h"
#include "hardware/BridgeCommandTransport.h"
#include "hardware/BridgeDataReceiver.h"
#include "hardware/MvlcCommandTransport.h"
#include "hardware/MvlcDataReceiver.h"

#include <utility>

namespace fidget {
namespace {

ControllerEndpointRequest ValidationRequest(
    const DirectEthernetEndpointRequest& request)
{
    return {
        ControllerEndpointKind::DirectEthernet,
        request.mvlcHost,
        request.mvlcCommandPort,
        {},
        {},
    };
}

ControllerEndpointRequest ValidationRequest(
    const SshBridgeEndpointRequest& request)
{
    return {
        ControllerEndpointKind::SshBridge,
        request.mvlcHost,
        request.mvlcCommandPort,
        request.sshDestination,
        request.remoteBridgeCommand,
    };
}

} // namespace

TransportSession::TransportSession(
    std::unique_ptr<ICommandTransport> commandTransport,
    std::unique_ptr<IDataReceiver> dataReceiver,
    std::shared_ptr<SshBridgeProcess> bridgeProcess)
    : commandTransport_(std::move(commandTransport))
    , dataReceiver_(std::move(dataReceiver))
    , bridgeProcess_(std::move(bridgeProcess))
{
}

TransportSession::~TransportSession()
{
    Close();
}

ICommandTransport* TransportSession::CommandTransport() const noexcept
{
    return commandTransport_.get();
}

IDataReceiver* TransportSession::DataReceiver() const noexcept
{
    return dataReceiver_.get();
}

std::string TransportSession::CapturedBridgeStderr() const
{
    return bridgeProcess_ ? bridgeProcess_->CapturedStderr() : std::string{};
}

void TransportSession::Close() noexcept
{
    if (dataReceiver_)
    {
        dataReceiver_->Close();
    }
    if (commandTransport_)
    {
        commandTransport_->Close();
    }
    if (bridgeProcess_)
    {
        bridgeProcess_->Stop();
    }
}

ITransportFactory::~ITransportFactory() = default;

TransportFactoryResult ITransportFactory::Create(
    const CrateProject& project)
{
    const auto validation = ValidateCrateProject(project);
    if (!validation.success)
        return {nullptr, validation.message};

    if (project.endpointKind == CrateProjectEndpointKind::Direct)
    {
        return Create(TransportEndpointRequest{
            DirectEthernetEndpointRequest{
                project.mvlcHost,
                project.mvlcCommandPort,
            },
        });
    }

    return Create(TransportEndpointRequest{
        SshBridgeEndpointRequest{
            project.mvlcHost,
            project.mvlcCommandPort,
            project.sshDestination,
            project.remoteBridgeCommand,
        },
    });
}

MvlcTransportFactory::MvlcTransportFactory()
    : MvlcTransportFactory(
        [](const std::string& destination,
           const std::string& remoteCommand,
           const std::string& mvlcHost,
           const std::uint16_t commandPort) {
            return SshBridgeProcess::StartSsh(
                destination,
                remoteCommand,
                mvlcHost,
                commandPort);
        })
{
}

MvlcTransportFactory::MvlcTransportFactory(
    SshBridgeStarter bridgeStarter)
    : bridgeStarter_(std::move(bridgeStarter))
{
}

TransportFactoryResult MvlcTransportFactory::Create(
    const TransportEndpointRequest& request)
{
    if (const auto* direct =
            std::get_if<DirectEthernetEndpointRequest>(&request))
    {
        const auto validation =
            ValidateControllerEndpoint(ValidationRequest(*direct));
        if (!validation.success)
            return {nullptr, validation.message};
        return {
            std::make_unique<TransportSession>(
                std::make_unique<MvlcCommandTransport>(),
                std::make_unique<MvlcDataReceiver>()),
            {},
        };
    }

    const auto& bridge = std::get<SshBridgeEndpointRequest>(request);
    const auto validation =
        ValidateControllerEndpoint(ValidationRequest(bridge));
    if (!validation.success)
        return {nullptr, validation.message};
    if (!bridgeStarter_)
    {
        return {nullptr, "The SSH bridge process starter is unavailable."};
    }
    auto started = bridgeStarter_(
        bridge.sshDestination,
        bridge.remoteBridgeCommand,
        bridge.mvlcHost,
        bridge.mvlcCommandPort);
    if (!started.success || !started.process)
    {
        return {
            nullptr,
            started.error.empty()
                ? "The SSH bridge process did not start."
                : std::move(started.error),
        };
    }

    auto process = std::move(started.process);
    const auto connection = process->Connection();
    if (!connection)
    {
        process->Stop();
        return {nullptr, "The SSH bridge process has no framed connection."};
    }
    return {
        std::make_unique<TransportSession>(
            std::make_unique<BridgeCommandTransport>(connection),
            std::make_unique<BridgeDataReceiver>(connection),
            std::move(process)),
        {},
    };
}

} // namespace fidget
