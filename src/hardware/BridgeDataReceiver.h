#ifndef FIDGET_HARDWARE_BRIDGE_DATA_RECEIVER_H
#define FIDGET_HARDWARE_BRIDGE_DATA_RECEIVER_H

#include "hardware/Transport.h"

#include <memory>

namespace fidget {

class BridgeConnection;

class BridgeDataReceiver final : public IDataReceiver
{
public:
    explicit BridgeDataReceiver(std::shared_ptr<BridgeConnection> connection);
    ~BridgeDataReceiver() override;

    BridgeDataReceiver(const BridgeDataReceiver&) = delete;
    BridgeDataReceiver& operator=(const BridgeDataReceiver&) = delete;

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
