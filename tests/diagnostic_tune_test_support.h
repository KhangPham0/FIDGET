#ifndef FIDGET_TESTS_DIAGNOSTIC_TUNE_TEST_SUPPORT_H
#define FIDGET_TESTS_DIAGNOSTIC_TUNE_TEST_SUPPORT_H

#include "core/ReadoutProtocol.h"
#include "core/VmeProtocol.h"
#include "hardware/DiagnosticAcquisitionOperation.h"
#include "vme_test_support.h"

#include "doctest/doctest.h"

#include <cstdint>
#include <vector>

namespace fidget::test {

inline constexpr std::uint32_t DiagnosticTestBase = 0x11000000U;
inline constexpr std::uint32_t DiagnosticTestToken = 0xA55A1234U;

inline DiagnosticAcquisitionPreparationResult MakeRunningDiagnosticSession()
{
    DiagnosticAcquisitionPreparationResult session;
    session.acquisition.state = DiagnosticAcquisitionState::Running;
    session.acquisition.baseAddress = DiagnosticTestBase;
    const auto plan = MakeMvlcSingleMdppReadoutPlan(
        DiagnosticTestBase, 3U);
    REQUIRE(plan.success);
    session.readoutPlan = plan.plan;
    REQUIRE_FALSE(session.readoutPlan.stackUploadWrites.empty());
    session.recoveryRecord.ownershipTokenRegister = static_cast<std::uint16_t>(
        session.readoutPlan.stackUploadWrites.back().address + 4U);
    session.recoveryRecord.ownershipTokenValue = DiagnosticTestToken;
    session.nextSuperReference = 0x7000U;
    session.nextStackReference = 0x9F000001U;
    return session;
}

inline std::vector<std::uint16_t> DiagnosticFingerprintAddresses(
    const DiagnosticAcquisitionPreparationResult& session)
{
    std::vector<std::uint16_t> addresses{
        DiagnosticDaqModeRegister,
        session.readoutPlan.stackTriggerRegister,
        session.readoutPlan.stackOffsetRegister,
    };
    for (const auto& write : session.readoutPlan.stackUploadWrites)
    {
        addresses.push_back(write.address);
    }
    addresses.push_back(session.recoveryRecord.ownershipTokenRegister);
    return addresses;
}

inline std::vector<std::uint32_t> DiagnosticFingerprintValues(
    const DiagnosticAcquisitionPreparationResult& session)
{
    std::vector<std::uint32_t> values{
        DiagnosticDaqEnableValue,
        session.readoutPlan.triggerValue,
        session.readoutPlan.stackMemoryOffset,
    };
    for (const auto& write : session.readoutPlan.stackUploadWrites)
    {
        values.push_back(write.value);
    }
    values.push_back(session.recoveryRecord.ownershipTokenValue);
    return values;
}

inline void QueueLocalBatchRead(
    FakeCommandTransport& transport,
    const std::uint16_t reference,
    const std::vector<std::uint16_t>& addresses,
    const std::vector<std::uint32_t>& values)
{
    REQUIRE(addresses.size() == values.size());
    const auto request = BuildMvlcLocalRegisterBatchReadRequest(
        reference, addresses.data(), addresses.size());
    REQUIRE(request.success);
    std::vector<std::uint32_t> frame{
        (static_cast<std::uint32_t>(MvlcSuperFrameType) << 24U)
            | static_cast<std::uint32_t>(1U + addresses.size() * 2U),
        MvlcReferenceWordCommand | reference,
    };
    for (std::size_t index = 0U; index < addresses.size(); ++index)
    {
        frame.push_back(MvlcReadLocalCommand | addresses[index]);
        frame.push_back(values[index]);
    }
    transport.QueueExchange({
        EncodeWords(request.words),
        {FakeReceiveAction::Datagram(MakeCommandPacket({frame}))},
    });
}

inline void QueueDiagnosticFingerprint(
    FakeCommandTransport& transport,
    const DiagnosticAcquisitionPreparationResult& session,
    const std::uint16_t reference)
{
    QueueLocalBatchRead(
        transport,
        reference,
        DiagnosticFingerprintAddresses(session),
        DiagnosticFingerprintValues(session));
}

inline void QueueLocalWrite(
    FakeCommandTransport& transport,
    TransactionReferences& references,
    const std::uint16_t address,
    const std::uint32_t value)
{
    const MvlcLocalRegisterWrite write{address, value};
    const auto request = BuildMvlcLocalRegisterWriteRequest(
        references.super, &write, 1U);
    REQUIRE(request.success);
    transport.QueueExchange({
        EncodeWords(request.words),
        {FakeReceiveAction::Datagram(
            MakeCommandPacket({MakeSuperFrame(references.super)}))},
    });
    ++references.super;
}

inline void QueueLocalRead(
    FakeCommandTransport& transport,
    TransactionReferences& references,
    const std::uint16_t address,
    const std::uint32_t value)
{
    QueueLocalBatchRead(
        transport,
        references.super,
        std::vector<std::uint16_t>{address},
        std::vector<std::uint32_t>{value});
    ++references.super;
}

inline void QueueDiagnosticPause(
    FakeCommandTransport& transport,
    TransactionReferences& references)
{
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + DiagnosticAcquisitionControlRegister,
        0U);
    QueueLocalWrite(transport, references, DiagnosticDaqModeRegister, 0U);
    QueueLocalRead(transport, references, DiagnosticDaqModeRegister, 0U);
}

inline void QueueDiagnosticResume(
    FakeCommandTransport& transport,
    TransactionReferences& references,
    const std::uint32_t daqModeReadback = 0x00000005U)
{
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + DiagnosticFifoResetRegister,
        1U);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + DiagnosticReadoutResetRegister,
        1U);
    QueueWrite(
        transport,
        references,
        DiagnosticTestBase + DiagnosticAcquisitionControlRegister,
        1U);
    QueueLocalWrite(
        transport,
        references,
        DiagnosticDaqModeRegister,
        DiagnosticDaqEnableValue);
    QueueLocalRead(
        transport,
        references,
        DiagnosticDaqModeRegister,
        daqModeReadback);
}

} // namespace fidget::test

#endif
