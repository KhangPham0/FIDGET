#ifndef FIDGET_HARDWARE_PREPARE_TUNING_SESSION_OPERATION_H
#define FIDGET_HARDWARE_PREPARE_TUNING_SESSION_OPERATION_H

#include "core/ApplicationStorage.h"
#include "core/RecoveryJournal.h"
#include "core/TargetModuleAddress.h"
#include "core/TunerTarget.h"
#include "hardware/ScpCaptureOperation.h"
#include "hardware/TransportFactory.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>

namespace fidget {

enum class PrepareTuningSessionOutcome
{
    NotRun,
    SnapshotCaptured,
    PreflightRefused,
    StorageUnavailable,
    RecoveryRecordExists,
    ControllerBecameActive,
    CaptureFailed,
    JournalPromotionFailed,
    Interrupted,
    TransportUnavailable,
};

enum class PrepareTuningSessionJournalPhase
{
    None,
    Prepared,
    SnapshotCaptured,
};

enum class PrepareTuningSessionCheckpointKind
{
    JournalPrepared,
    DaqIdleVerified,
    GlobalRegisterRead,
    SelectorSettled,
    BankedRegisterRead,
    SelectorParked,
    CaptureCompleted,
    BeforeJournalPromotion,
    JournalPromoted,
};

struct PrepareTuningSessionCheckpoint
{
    PrepareTuningSessionCheckpointKind kind =
        PrepareTuningSessionCheckpointKind::JournalPrepared;
    std::optional<std::uint16_t> quad;
    std::uint16_t registerOffset = 0U;
    std::size_t ordinal = 0U;
};

using PrepareTuningSessionCheckpointGate =
    std::function<bool(const PrepareTuningSessionCheckpoint&)>;

struct PrepareTuningSessionRuntime
{
    // Production uses the real selector sleep. Tests record the exact delay
    // boundary without waiting.
    ScpCaptureDelay delay;
    // A false result models abrupt loss immediately after the named completed
    // step. The operation sends no cleanup traffic after that point.
    PrepareTuningSessionCheckpointGate checkpoint;
    // Used for both the exclusive Prepared write and atomic promotion. The
    // full durability runtime keeps fault injection aligned with the legacy
    // CX-12F path: same-device proof, file/directory sync, and no-delete
    // replacement all use the shared implementation.
    TunerRecoveryJournalSaveRuntime journalRuntime;
};

struct PrepareTuningSessionRequest
{
    TransportEndpointRequest endpoint;
    TargetModuleAddress targetAddress;
    ApplicationStoragePaths storagePaths;
    std::filesystem::path recoveryJournalPath;
};

struct PrepareTuningSessionResult
{
    PrepareTuningSessionOutcome outcome =
        PrepareTuningSessionOutcome::NotRun;
    PrepareTuningSessionJournalPhase journalPhase =
        PrepareTuningSessionJournalPhase::None;
    std::string message;
    std::string recoveryJournalPath;
    // These evidence flags describe step 1 only. In particular, its
    // noVmeOrModuleSettingWritesSent fact is not a claim about the later,
    // journal-authorized selector writes recorded by this result.
    TargetProbeResult preflight;
    Fw2051ScpConfigurationSnapshot liveRestoreSnapshot;
    bool temporaryConnectionOpened = false;
    bool temporaryConnectionClosed = false;
    bool journalCreatedExclusively = false;
    bool journalPromotedAtomically = false;
    bool controllerBecameActive = false;
    std::size_t daqIdleChecks = 0U;
    std::size_t completedCheckpoints = 0U;
    std::optional<PrepareTuningSessionCheckpoint> interruptedAt;
};

// Revalidates the exact supported hardware and stopped states before touching
// application storage. The first VME-bus write is a selector write and cannot
// occur until an exclusive, checksum-valid identity-only v5 Prepared record is
// durable. No frontend data register is written by this operation, so the
// 20-us post-frontend-write settle is not applicable; every selector write
// still receives its required 50-us settle before any read or checkpoint.
[[nodiscard]] PrepareTuningSessionResult PrepareTuningSession(
    ITransportFactory& transportFactory,
    const PrepareTuningSessionRequest& request,
    const std::atomic<bool>& cancellationRequested,
    const PrepareTuningSessionRuntime& runtime = {});

} // namespace fidget

#endif
