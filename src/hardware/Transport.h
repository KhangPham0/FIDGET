#ifndef FIDGET_HARDWARE_TRANSPORT_H
#define FIDGET_HARDWARE_TRANSPORT_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace fidget {

struct TransportOperationResult
{
    bool success = false;
    std::string error;
};

enum class TransportReceiveStatus
{
    Received,
    Timeout,
    Error,
};

struct TransportReceiveResult
{
    TransportReceiveStatus status = TransportReceiveStatus::Error;
    std::size_t bytesReceived = 0U;
    std::string error;
};

class ICommandTransport
{
public:
    virtual ~ICommandTransport();

    [[nodiscard]] virtual TransportOperationResult Open(
        const std::string& host,
        std::uint16_t port) = 0;
    [[nodiscard]] virtual TransportOperationResult Send(
        const std::byte* data,
        std::size_t size) = 0;
    [[nodiscard]] virtual TransportReceiveResult Receive(
        std::byte* buffer,
        std::size_t capacity,
        int timeoutMilliseconds) = 0;
    virtual void Close() noexcept = 0;
};

class IDataReceiver
{
public:
    virtual ~IDataReceiver();

    [[nodiscard]] virtual TransportOperationResult Open(
        const std::string& host,
        std::uint16_t port) = 0;
    [[nodiscard]] virtual TransportOperationResult Send(
        const std::byte* data,
        std::size_t size) = 0;
    [[nodiscard]] virtual TransportReceiveResult Receive(
        std::byte* buffer,
        std::size_t capacity,
        int timeoutMilliseconds) = 0;
    virtual void Close() noexcept = 0;
};

} // namespace fidget

#endif
