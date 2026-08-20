#ifndef FIDGET_HARDWARE_BRIDGE_COMMAND_TRANSPORT_H
#define FIDGET_HARDWARE_BRIDGE_COMMAND_TRANSPORT_H

#include "hardware/Transport.h"

#include <memory>

namespace fidget {

class BridgeConnection;

class BridgeCommandTransport final : public ICommandTransport
{
public:
    explicit BridgeCommandTransport(
        std::shared_ptr<BridgeConnection> connection);
    ~BridgeCommandTransport() override;

    BridgeCommandTransport(const BridgeCommandTransport&) = delete;
    BridgeCommandTransport& operator=(const BridgeCommandTransport&) = delete;

    [[nodiscard]] TransportOperationResult Open(
        const std::string& host,
        std::uint16_t port) override;
    [[nodiscard]] TransportOperationResult Send(
        const std::byte* data,
        std::size_t size) override;
    [[nodiscard]] TransportReceiveResult Receive(
        std::byte* buffer,
        std::size_t capacity,
        int timeoutMilliseconds) override;
    void Close() noexcept override;

private:
    std::shared_ptr<BridgeConnection> connection_;
    bool open_ = false;
};

} // namespace fidget

#endif
