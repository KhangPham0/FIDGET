#ifndef FIDGET_HARDWARE_MVLC_DATA_RECEIVER_H
#define FIDGET_HARDWARE_MVLC_DATA_RECEIVER_H

#include "hardware/Transport.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace fidget {

class MvlcDataReceiver final : public IDataReceiver
{
public:
    MvlcDataReceiver() = default;
    ~MvlcDataReceiver() override;

    MvlcDataReceiver(const MvlcDataReceiver&) = delete;
    MvlcDataReceiver& operator=(const MvlcDataReceiver&) = delete;

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
