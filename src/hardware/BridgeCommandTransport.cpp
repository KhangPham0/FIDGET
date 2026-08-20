#include "hardware/BridgeCommandTransport.h"

#include "hardware/BridgeConnection.h"

#include <utility>

namespace fidget {

BridgeCommandTransport::BridgeCommandTransport(
    std::shared_ptr<BridgeConnection> connection)
    : connection_(std::move(connection))
{
}

BridgeCommandTransport::~BridgeCommandTransport()
{
    Close();
}

TransportOperationResult BridgeCommandTransport::Open(
    const std::string& host, const std::uint16_t port)
{
    Close();
    if (!connection_)
    {
        return {false, "Bridge command transport has no connection"};
    }

    const auto result = connection_->OpenChannel(
        BridgeChannel::Command, host, port);
    open_ = result.success;
    return result;
}

TransportOperationResult BridgeCommandTransport::Send(
    const std::byte* data, const std::size_t size)
{
    if (!open_ || !connection_)
    {
        return {false, "send: bridge command transport is closed"};
    }
    return connection_->Send(BridgeChannel::Command, data, size);
}

TransportReceiveResult BridgeCommandTransport::Receive(
    std::byte* buffer,
    const std::size_t capacity,
    const int timeoutMilliseconds)
{
    if (!open_ || !connection_)
    {
        return {
            TransportReceiveStatus::Error,
            0U,
            "receive: bridge command transport is closed",
        };
    }
    return connection_->Receive(
        BridgeChannel::Command,
        buffer,
        capacity,
        timeoutMilliseconds);
}

void BridgeCommandTransport::Close() noexcept
{
    if (!open_ || !connection_)
    {
        return;
    }
    open_ = false;
    connection_->CloseChannel(BridgeChannel::Command);
}

} // namespace fidget
