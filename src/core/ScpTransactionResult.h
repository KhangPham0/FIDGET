#ifndef FIDGET_CORE_SCP_TRANSACTION_RESULT_H
#define FIDGET_CORE_SCP_TRANSACTION_RESULT_H

#include "core/ScpTransactionPlan.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fidget {

enum class ScpSingleRepairState
{
    NotRun,
    Applying,
    Passed,
    Failed,
};

struct ScpSingleRepairRequest
{
    std::uint32_t baseAddress = 0U;
    std::uint16_t quad = 0U;
    std::uint16_t registerOffset = 0U;
    std::uint16_t expectedLiveValue = 0U;
    std::uint16_t profileValue = 0U;
};

struct ScpSingleRepairResult
{
    ScpSingleRepairState state = ScpSingleRepairState::NotRun;
    std::string message = "No SCP profile repair has been requested";
    std::string settingName = "SCP parameter";
    std::string dependencyName;
    std::uint32_t baseAddress = 0U;
    std::uint16_t selectedQuad = 0U;
    std::uint16_t registerOffset = 0U;
    std::uint16_t expectedLiveValue = 0U;
    std::uint16_t profileValue = 0U;
    std::uint16_t capturedLiveValue = 0U;
    std::uint16_t appliedReadback = 0U;
    std::uint16_t rollbackReadback = 0U;
    std::uint16_t dependencyValue = 0U;
    bool dependencyChecked = false;
    bool liveValueCaptured = false;
    bool writeAttempted = false;
    bool writeVerified = false;
    bool rollbackAttempted = false;
    bool rollbackVerified = false;
    bool selectorParkedAtQuadZero = false;
    bool profileValueRetained = false;
};

enum class ScpBulkApplyState
{
    NotRun,
    Applying,
    RollingBack,
    Passed,
    Failed,
};

struct ScpAppliedValueResult
{
    int quad = -1;
    std::uint16_t registerOffset = 0U;
    std::string settingName;
    std::uint16_t expectedValue = 0U;
    std::uint16_t profileValue = 0U;
    std::uint16_t appliedReadback = 0U;
    std::uint16_t rollbackReadback = 0U;
    bool writeAttempted = false;
    bool writeVerified = false;
    bool rollbackAttempted = false;
    bool rollbackVerified = false;
    bool profileValueRetained = false;
};

struct ScpBulkApplyResult
{
    ScpBulkApplyState state = ScpBulkApplyState::NotRun;
    std::string message =
        "No complete SCP profile application has been requested";
    std::uint32_t baseAddress = 0U;
    std::size_t valuesCompared = 0U;
    std::size_t configurationDifferences = 0U;
    std::size_t plannedWrites = 0U;
    std::size_t writesAttempted = 0U;
    std::size_t writesVerified = 0U;
    std::size_t rollbackWritesAttempted = 0U;
    std::size_t rollbackWritesVerified = 0U;
    bool fullPreflightMatched = false;
    bool moduleStopSent = false;
    bool moduleStopVerified = false;
    bool moduleLeftStopped = false;
    bool rollbackAttempted = false;
    bool rollbackVerified = false;
    bool selectorParkedAtQuadZero = false;
    bool fifoResetSent = false;
    bool readoutResetSent = false;
    bool profileValuesRetained = false;
    std::vector<ScpAppliedValueResult> values;
};

} // namespace fidget

#endif
