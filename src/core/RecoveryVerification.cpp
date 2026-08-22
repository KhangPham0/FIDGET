#include "core/RecoveryVerification.h"

#include "core/ReadoutProtocol.h"

#include <cstdio>
#include <string>

namespace fidget {
namespace {

std::string Hexadecimal32(const std::uint32_t value)
{
    char buffer[16]{};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "0x%08X",
        static_cast<unsigned>(value));
    return buffer;
}

TunerRecoveryFingerprintEvaluation Mismatch(
    std::string field,
    const std::uint32_t expected,
    const std::uint32_t actual)
{
    TunerRecoveryFingerprintEvaluation result;
    result.firstMismatchedField = std::move(field);
    result.message = result.firstMismatchedField + " mismatched: expected "
        + Hexadecimal32(expected) + ", read " + Hexadecimal32(actual)
        + ". No cleanup write is allowed.";
    return result;
}

} // namespace

TunerRecoveryFingerprintExpectation
BuildTunerRecoveryFingerprintExpectation(const TunerRecoveryRecord& record)
{
    TunerRecoveryFingerprintExpectation result;
    const auto readout = MakeMvlcSingleMdppReadoutPlan(
        record.mdppBaseAddress, record.mdppIrqLevel);
    if (!readout.success)
    {
        result.message =
            "The recovery journal cannot reconstruct the diagnostic stack: "
            + readout.error;
        return result;
    }
    if (readout.plan.stackUploadWrites.size() != 7U)
    {
        result.message =
            "The reconstructed diagnostic stack does not contain seven words.";
        return result;
    }

    result.addresses[0] = TunerRecoveryDaqModeRegister;
    result.addresses[1] = record.stackTriggerRegister;
    result.addresses[2] = record.stackOffsetRegister;
    result.values[1] = record.stackTriggerValue;
    result.values[2] = record.stackOffsetValue;

    std::size_t index = 3U;
    for (const auto& stackWord : readout.plan.stackUploadWrites)
    {
        result.addresses[index] = stackWord.address;
        result.values[index] = stackWord.value;
        ++index;
    }
    result.addresses[index] = record.ownershipTokenRegister;
    result.values[index] = record.ownershipTokenValue;
    result.success = true;
    result.message = "Built the journaled tuner fingerprint.";
    return result;
}

TunerRecoveryFingerprintEvaluation EvaluateTunerRecoveryFingerprint(
    const TunerRecoveryRecord& record,
    const TunerRecoveryLiveFingerprint& live)
{
    if (live.mvlcHardwareId != record.mvlcHardwareId)
    {
        return Mismatch(
            "MVLC hardware ID",
            record.mvlcHardwareId,
            live.mvlcHardwareId);
    }
    if (live.mvlcFirmwareRevision != record.mvlcFirmwareRevision)
    {
        return Mismatch(
            "MVLC firmware revision",
            record.mvlcFirmwareRevision,
            live.mvlcFirmwareRevision);
    }

    TunerRecoveryFingerprintEvaluation result;
    if (live.values[0] == 0U)
    {
        if (record.previewRestoreRequired
            || record.sourceRestoreRequired)
        {
            result.verdict =
                TunerRecoveryFingerprintVerdict::IdleWithRestoration;
            result.message =
                "MVLC DAQ mode is zero, but the recovery journal records a pending restoration. The journal must be retained until the original module value is verified or restored.";
        }
        else
        {
            result.verdict =
                TunerRecoveryFingerprintVerdict::AlreadyClean;
            result.message =
                "MVLC DAQ mode is zero and no restoration is pending. The stale recovery journal may be removed without a hardware write; compare the module afterward.";
        }
        return result;
    }

    const auto expected = BuildTunerRecoveryFingerprintExpectation(record);
    if (!expected.success)
    {
        result.firstMismatchedField = "recovery journal";
        result.message = expected.message + " No cleanup write is allowed.";
        return result;
    }

    static constexpr const char* FixedFieldNames[]{
        "MVLC DAQ mode",
        "Diagnostic stack trigger",
        "Diagnostic stack offset",
    };
    for (std::size_t index = 1U; index < live.values.size(); ++index)
    {
        if (live.values[index] == expected.values[index])
        {
            continue;
        }
        if (index < 3U)
        {
            return Mismatch(
                FixedFieldNames[index],
                expected.values[index],
                live.values[index]);
        }
        if (index < 10U)
        {
            return Mismatch(
                "Diagnostic stack word " + std::to_string(index - 3U),
                expected.values[index],
                live.values[index]);
        }
        return Mismatch(
            "Unique tuner ownership token",
            expected.values[index],
            live.values[index]);
    }

    result.verdict = TunerRecoveryFingerprintVerdict::OrphanConfirmed;
    result.message =
        "The live MVLC identity and complete unique tuner fingerprint match the recovery journal.";
    return result;
}

} // namespace fidget
