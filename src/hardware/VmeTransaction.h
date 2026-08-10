#ifndef FIDGET_HARDWARE_VME_TRANSACTION_H
#define FIDGET_HARDWARE_VME_TRANSACTION_H

#include "core/VmeProtocol.h"
#include "hardware/Transport.h"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace fidget {

inline constexpr int MvlcTransactionAttemptCount = 3;
inline constexpr int MvlcMaximumResponseDatagrams = 8;
inline constexpr std::size_t MvlcCommandResponseBufferSize = 9000U;
inline constexpr int MvlcCommandResponseTimeoutMilliseconds = 500;

[[nodiscard]] MvlcVmeReadResult ReadVmeD16(
    ICommandTransport& transport,
    std::uint32_t address,
    std::uint16_t& nextSuperReference,
    std::uint32_t& nextStackReference,
    const std::atomic<bool>& cancellationRequested);

[[nodiscard]] MvlcVmeWriteResult WriteVmeD16(
    ICommandTransport& transport,
    std::uint32_t address,
    std::uint16_t value,
    std::uint16_t& nextSuperReference,
    std::uint32_t& nextStackReference,
    const std::atomic<bool>& cancellationRequested);

[[nodiscard]] MvlcLocalWriteResult WriteLocalRegisters(
    ICommandTransport& transport,
    const MvlcLocalRegisterWrite* writes,
    std::size_t writeCount,
    std::uint16_t& nextSuperReference,
    const std::atomic<bool>& cancellationRequested);

} // namespace fidget

#endif
