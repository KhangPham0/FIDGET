#include "hardware/BridgeDataReceiver.h"

#include "hardware/BridgeConnection.h"

#include <utility>

namespace fidget {

BridgeDataReceiver::BridgeDataReceiver(
    std::shared_ptr<BridgeConnection> connection)
    : connection_(std::move(connection))
{
}

BridgeDataReceiver::~BridgeDataReceiver()
{
    Close();
}

TransportOperationResult BridgeDataReceiver::Open(
    const std::string& host, const std::uint16_t port)
{
    Close();
    if (!connection_)
    {
        return {false, "Bridge data receiver has no connection"};
    }

    const auto result = connection_->OpenChannel(
        BridgeChannel::Data, host, port);
    open_ = result.success;
    return result;
}

TransportOperationResult BridgeDataReceiver::Send(
    const std::byte* data, const std::size_t size)
{
    if (!open_ || !connection_)
    {
        return {false, "data send: bridge data receiver is closed"};
    }
    return connection_->Send(BridgeChannel::Data, data, size);
}

TransportReceiveResult BridgeDataReceiver::Receive(
    std::byte* buffer,
    const std::size_t capacity,
    const int timeoutMilliseconds)
{
    if (!open_ || !connection_)
    {
        return {
            TransportReceiveStatus::Error,
            0U,
            "data receive: bridge data receiver is closed",
        };
    }
    return connection_->Receive(
        BridgeChannel::Data,
        buffer,
        capacity,
        timeoutMilliseconds);
}

void BridgeDataReceiver::Close() noexcept
{
    if (!open_ || !connection_)
    {
        return;
    }
    open_ = false;
    connection_->CloseChannel(BridgeChannel::Data);
}

} // namespace fidget
