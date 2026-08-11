#ifndef FIDGET_CORE_STARTUP_PREPARATION_H
#define FIDGET_CORE_STARTUP_PREPARATION_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fidget {

inline constexpr std::size_t Fw2051StartupPreparationRegisterCount = 8U;

struct Fw2051StartupPreparationDefinition
{
    std::uint16_t registerOffset;
    const char* name;
    std::uint16_t targetValue;
};

struct Fw2051StartupPreparationMismatch
{
    std::uint16_t registerOffset = 0U;
    const char* name = "";
    std::uint16_t currentValue = 0U;
    std::uint16_t targetValue = 0U;
};

enum class StartupPreparationState
{
    NotRun,
    Preparing,
    RollingBack,
    Passed,
    Failed,
};

struct StartupPreparationRegisterResult
{
    std::uint16_t registerOffset = 0U;
    std::string name;
    std::uint16_t originalValue = 0U;
    std::uint16_t targetValue = 0U;
    std::uint16_t appliedReadback = 0U;
    std::uint16_t rollbackReadback = 0U;
    bool changeRequired = false;
    bool writeAttempted = false;
    bool writeVerified = false;
    bool rollbackAttempted = false;
    bool rollbackVerified = false;
};

struct StartupPreparationResult
{
    StartupPreparationState state = StartupPreparationState::NotRun;
    std::string message =
        "No module-wide startup preparation has been requested";
    std::uint32_t baseAddress = 0U;
    std::uint16_t hardwareId = 0U;
    std::uint16_t firmwareRevision = 0U;
    std::uint16_t originalAcquisitionValue = 0U;
    std::size_t settingsRead = 0U;
    std::size_t changedSettings = 0U;
    std::size_t writesAttempted = 0U;
    std::size_t writesVerified = 0U;
    std::size_t rollbackWritesAttempted = 0U;
    std::size_t rollbackWritesVerified = 0U;
    bool strictFirmwareAccepted = false;
    bool moduleStopSent = false;
    bool moduleStopVerified = false;
    bool rollbackAttempted = false;
    bool rollbackVerified = false;
    bool fifoResetSent = false;
    bool readoutResetSent = false;
    bool moduleLeftStopped = false;
    bool experimentSettingsPreserved = true;
    std::vector<StartupPreparationRegisterResult> registers;
};

extern const std::array<
    Fw2051StartupPreparationDefinition,
    Fw2051StartupPreparationRegisterCount>
    Fw2051StartupPreparationRegisterTable;

[[nodiscard]] std::vector<Fw2051StartupPreparationMismatch>
FindFw2051StartupPreparationMismatches(
    const std::array<
        std::uint16_t,
        Fw2051StartupPreparationRegisterCount>& values);

} // namespace fidget

#endif
