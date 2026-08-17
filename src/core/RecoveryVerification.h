#ifndef FIDGET_CORE_RECOVERY_VERIFICATION_H
#define FIDGET_CORE_RECOVERY_VERIFICATION_H

#include "core/RecoveryJournal.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace fidget {

inline constexpr std::size_t TunerRecoveryFingerprintRegisterCount = 11U;
inline constexpr std::uint16_t TunerRecoveryDaqModeRegister = 0x1300U;

struct TunerRecoveryFingerprintExpectation
{
    bool success = false;
    std::string message;
    std::array<std::uint16_t,
               TunerRecoveryFingerprintRegisterCount> addresses{};
    std::array<std::uint32_t,
               TunerRecoveryFingerprintRegisterCount> values{};
};

struct TunerRecoveryLiveFingerprint
{
    std::uint32_t mvlcHardwareId = 0U;
    std::uint32_t mvlcFirmwareRevision = 0U;
    std::array<std::uint32_t,
               TunerRecoveryFingerprintRegisterCount> values{};
};

enum class TunerRecoveryFingerprintVerdict
{
    OrphanConfirmed,
    ForeignOrMismatched,
    AlreadyClean,
};

struct TunerRecoveryFingerprintEvaluation
{
    TunerRecoveryFingerprintVerdict verdict =
        TunerRecoveryFingerprintVerdict::ForeignOrMismatched;
    std::string firstMismatchedField;
    std::string message;
};

[[nodiscard]] TunerRecoveryFingerprintExpectation
BuildTunerRecoveryFingerprintExpectation(const TunerRecoveryRecord& record);

[[nodiscard]] TunerRecoveryFingerprintEvaluation
EvaluateTunerRecoveryFingerprint(
    const TunerRecoveryRecord& record,
    const TunerRecoveryLiveFingerprint& live);

} // namespace fidget

#endif
