#ifndef FIDGET_CORE_RECOVERY_VERIFICATION_H
#define FIDGET_CORE_RECOVERY_VERIFICATION_H

#include "core/RecoveryJournal.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace fidget {

inline constexpr std::size_t TunerRecoveryFingerprintRegisterCount = 11U;
inline constexpr std::uint16_t TunerRecoveryDaqModeRegister = 0x1300U;
inline constexpr std::uint16_t TunerRecoveryMvlcHardwareIdRegister = 0x6008U;
inline constexpr std::uint16_t TunerRecoveryMvlcFirmwareRegister = 0x600EU;

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
    IdleWithRestoration,
};

struct TunerRecoveryFingerprintEvaluation
{
    TunerRecoveryFingerprintVerdict verdict =
        TunerRecoveryFingerprintVerdict::ForeignOrMismatched;
    std::string firstMismatchedField;
    std::string message;
};

enum class DiagnosticOrphanRecoveryState
{
    NotRun,
    Recovering,
    Recovered,
    AlreadyClean,
    ForeignOrMismatched,
    Failed,
};

struct DiagnosticOrphanRecoveryStep
{
    std::string name;
    bool success = false;
    std::string message;
};

struct DiagnosticOrphanRecoveryResult
{
    DiagnosticOrphanRecoveryState state =
        DiagnosticOrphanRecoveryState::NotRun;
    std::string message = "No orphan recovery has been requested";
    TunerRecoveryFingerprintEvaluation fingerprint;
    std::vector<DiagnosticOrphanRecoveryStep> steps;
    std::size_t isolatedModulesRecovered = 0U;
    bool hardwareWriteSent = false;
    bool targetStopped = false;
    bool previewRestoreAttempted = false;
    bool previewRestoreVerified = false;
    bool sourceRestoreAttempted = false;
    bool sourceRestoreVerified = false;
    bool mvlcCleanupVerified = false;
    bool targetReset = false;
    bool journalRemoved = false;
};

[[nodiscard]] TunerRecoveryFingerprintExpectation
BuildTunerRecoveryFingerprintExpectation(const TunerRecoveryRecord& record);

[[nodiscard]] TunerRecoveryFingerprintEvaluation
EvaluateTunerRecoveryFingerprint(
    const TunerRecoveryRecord& record,
    const TunerRecoveryLiveFingerprint& live);

} // namespace fidget

#endif
