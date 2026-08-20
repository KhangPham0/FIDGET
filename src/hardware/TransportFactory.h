#ifndef FIDGET_HARDWARE_TRANSPORT_FACTORY_H
#define FIDGET_HARDWARE_TRANSPORT_FACTORY_H

#include "core/CrateProject.h"
#include "hardware/SshBridgeProcess.h"
#include "hardware/Transport.h"

#include <functional>
#include <memory>
#include <string>

namespace fidget {

class TransportSession
{
public:
    TransportSession(
        std::unique_ptr<ICommandTransport> commandTransport,
        std::unique_ptr<IDataReceiver> dataReceiver,
        std::shared_ptr<SshBridgeProcess> bridgeProcess = {});
    ~TransportSession();

    TransportSession(const TransportSession&) = delete;
    TransportSession& operator=(const TransportSession&) = delete;

    [[nodiscard]] ICommandTransport* CommandTransport() const noexcept;
    [[nodiscard]] IDataReceiver* DataReceiver() const noexcept;
    [[nodiscard]] std::string CapturedBridgeStderr() const;
    void Close() noexcept;

private:
    std::unique_ptr<ICommandTransport> commandTransport_;
    std::unique_ptr<IDataReceiver> dataReceiver_;
    std::shared_ptr<SshBridgeProcess> bridgeProcess_;
};

struct TransportFactoryResult
{
    std::unique_ptr<TransportSession> session;
    std::string error;
};

class ITransportFactory
{
public:
    virtual ~ITransportFactory();

    [[nodiscard]] virtual TransportFactoryResult Create(
        const CrateProject& project) = 0;
};

using SshBridgeStarter = std::function<SshBridgeProcessStartResult(
    const std::string& destination,
    const std::string& remoteCommand,
    const std::string& mvlcHost,
    std::uint16_t commandPort)>;

class MvlcTransportFactory final : public ITransportFactory
{
public:
    MvlcTransportFactory();
    explicit MvlcTransportFactory(SshBridgeStarter bridgeStarter);

    [[nodiscard]] TransportFactoryResult Create(
        const CrateProject& project) override;

private:
    SshBridgeStarter bridgeStarter_;
};

} // namespace fidget

#endif
