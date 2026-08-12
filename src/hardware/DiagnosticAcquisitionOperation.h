#ifndef FIDGET_HARDWARE_DIAGNOSTIC_ACQUISITION_OPERATION_H
#define FIDGET_HARDWARE_DIAGNOSTIC_ACQUISITION_OPERATION_H

#include "core/Acquisition.h"
#include "core/ReadoutProtocol.h"
#include "core/RecoveryJournal.h"
#include "hardware/ScpCaptureOperation.h"
#include "hardware/Transport.h"

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace fidget {

inline constexpr std::uint16_t DiagnosticHardwareIdRegister = 0x6008U;
inline constexpr std::uint16_t DiagnosticIrqLevelRegister = 0x6010U;
inline constexpr std::uint16_t DiagnosticReadoutResetRegister = 0x6034U;
inline constexpr std::uint16_t DiagnosticAcquisitionControlRegister = 0x603AU;
inline constexpr std::uint16_t DiagnosticFifoResetRegister = 0x603CU;
inline constexpr std::uint16_t DiagnosticOutputFormatRegister = 0x6044U;
inline constexpr std::uint16_t DiagnosticDaqModeRegister = 0x1300U;
inline constexpr std::uint32_t DiagnosticDaqEnableValue = 0x000FU;
inline constexpr int DiagnosticDataReceiveTimeoutMilliseconds = 100;

struct DiagnosticAcquisitionPreparationRequest
{
    std::string host;
    std::uint16_t commandPort = 32768U;
    std::uint32_t mvlcHardwareId = 0U;
    std::uint32_t mvlcFirmwareRevision = 0U;
    std::uint32_t targetBaseAddress = 0U;
    std::uint16_t requestedChannel = 0U;
    std::vector<std::uint32_t> configuredModuleBaseAddresses;
    std::string recoveryJournalPath;

    // Zero asks the operation to generate a fresh nonzero token. Tests may
    // provide a fixed value to pin the complete journal and wire sequence.
    std::uint32_t ownershipTokenValue = 0U;
};

struct DiagnosticAcquisitionPreparationResult
{
    DiagnosticAcquisitionResult acquisition;
    MvlcSingleMdppReadoutPlan readoutPlan;
    TunerRecoveryRecord recoveryRecord;
    std::uint16_t nextSuperReference = 0x5000U;
    std::uint32_t nextStackReference = 0x9E000001U;
};

[[nodiscard]] DiagnosticAcquisitionPreparationResult
PrepareDiagnosticAcquisition(
    ICommandTransport& transport,
    const DiagnosticAcquisitionPreparationRequest& request,
    const std::atomic<bool>& cancellationRequested,
    const ScpCaptureOwnershipGate& ownershipGate);

[[nodiscard]] DiagnosticAcquisitionPreparationResult
StartPreparedDiagnosticAcquisition(
    ICommandTransport& commandTransport,
    IDataReceiver& dataReceiver,
    DiagnosticAcquisitionPreparationResult prepared,
    const DiagnosticAcquisitionPreparationRequest& request,
    const std::atomic<bool>& cancellationRequested);

[[nodiscard]] DiagnosticFingerprintResult
VerifyDiagnosticOwnershipFingerprint(
    ICommandTransport& commandTransport,
    const DiagnosticAcquisitionPreparationResult& prepared,
    std::uint16_t& nextReference,
    const std::atomic<bool>& cancellationRequested);

[[nodiscard]] DiagnosticAcquisitionPreparationResult
StopDiagnosticAcquisition(
    ICommandTransport& commandTransport,
    IDataReceiver& dataReceiver,
    DiagnosticAcquisitionPreparationResult prepared,
    const DiagnosticAcquisitionPreparationRequest& request,
    const std::atomic<bool>& cancellationRequested);

} // namespace fidget

#endif
