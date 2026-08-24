#ifndef FIDGET_CORE_SCP_CONFIGURATION_H
#define FIDGET_CORE_SCP_CONFIGURATION_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fidget {

inline constexpr std::uint16_t Mdpp32HardwareId = 0x5007U;
inline constexpr std::uint16_t Mdpp32AlternateHardwareId = 0x500CU;
inline constexpr std::uint16_t Mdpp32ScpFirmwareRevisionFw2051 = 0x2051U;

// Read-only recognition and write authorization are intentionally separate.
// The latter may widen only after the corresponding hardware-acceptance
// checklist evidence is recorded.
inline constexpr std::array<std::uint16_t, 2U>
    Mdpp32ReadOnlyAcceptedHardwareIds{{
        Mdpp32HardwareId,
        Mdpp32AlternateHardwareId,
    }};
inline constexpr std::array<std::uint16_t, 1U>
    Mdpp32WriteApprovedHardwareIds{{Mdpp32HardwareId}};

constexpr bool IsSupportedMdpp32HardwareId(const std::uint16_t hardwareId)
{
    for (const auto accepted : Mdpp32ReadOnlyAcceptedHardwareIds)
    {
        if (hardwareId == accepted)
            return true;
    }
    return false;
}

constexpr bool IsWriteApprovedMdpp32HardwareId(
    const std::uint16_t hardwareId)
{
    for (const auto accepted : Mdpp32WriteApprovedHardwareIds)
    {
        if (hardwareId == accepted)
            return true;
    }
    return false;
}

inline constexpr std::uint16_t Fw2051AcquisitionControlRegister = 0x603AU;
inline constexpr std::uint16_t Fw2051FifoResetRegister = 0x603CU;
inline constexpr std::uint16_t Fw2051ReadoutResetRegister = 0x6034U;
inline constexpr std::uint16_t Fw2051ScpSelectorRegister = 0x6100U;
inline constexpr std::uint16_t Fw2051StopAcquisitionValue = 0U;
inline constexpr std::uint16_t Fw2051ResetCommandValue = 1U;
inline constexpr std::size_t Fw2051ScpQuadCount = 8U;
inline constexpr std::size_t Fw2051ScpBankedSettingCount = 17U;
inline constexpr std::size_t Fw2051ScpConfigurationValueCount =
    5U + Fw2051ScpQuadCount * Fw2051ScpBankedSettingCount;

enum class ScpConfigurationState
{
    NotRun,
    Reading,
    Complete,
    Failed,
};

struct Fw2051ScpQuadConfiguration
{
    std::uint16_t quad = 0;
    std::uint16_t timingFilter = 0;
    std::array<std::uint16_t, 4> poleZero{};
    std::uint16_t gain = 0;
    std::array<std::uint16_t, 4> thresholds{};
    std::uint16_t shapingTime = 0;
    std::uint16_t baselineRestorer = 0;
    std::uint16_t resetTime = 0;
    std::uint16_t signalRiseTime = 0;
    std::uint16_t preSamples = 0;
    std::uint16_t totalSamples = 0;
    std::uint16_t sampleConfiguration = 0;
};

struct Fw2051ScpSelectorWriteRecord
{
    std::uint16_t registerOffset = Fw2051ScpSelectorRegister;
    std::uint16_t value = 0U;
    bool parkingWrite = false;
    bool success = false;
    std::string message;
};

struct Fw2051ScpConfigurationSnapshot
{
    ScpConfigurationState state = ScpConfigurationState::NotRun;
    std::string message = "No SCP configuration snapshot has been read";
    std::uint32_t baseAddress = 0;
    std::uint16_t hardwareId = 0;
    std::uint16_t firmwareRevision = 0;
    std::uint16_t irqLevel = 0;
    std::uint16_t outputFormat = 0;
    bool selectorParkedAtQuadZero = false;
    std::vector<Fw2051ScpSelectorWriteRecord> selectorWrites;
    std::vector<Fw2051ScpQuadConfiguration> quads;
};

} // namespace fidget

#endif
