#ifndef FIDGET_HARDWARE_MVLC_COMMAND_TRANSPORT_H
#define FIDGET_HARDWARE_MVLC_COMMAND_TRANSPORT_H

#include "hardware/Transport.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace fidget {

class MvlcCommandTransport final : public ICommandTransport
{
public:
    MvlcCommandTransport() = default;
    ~MvlcCommandTransport() override;

    MvlcCommandTransport(const MvlcCommandTransport&) = delete;
    MvlcCommandTransport& operator=(const MvlcCommandTransport&) = delete;

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

    [[nodiscard]] bool IsOpen() const noexcept;

private:
    int socketDescriptor_ = -1;
};

} // namespace fidget

#endif
