#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/ActivityLogFile.h"
#include "core/VmeProtocol.h"
#include "core/RecoveryVerification.h"
#include "core/ScpRegistry.h"
#include "core/StartupAudit.h"
#include "fake_command_transport.h"
#include "hardware/OwnershipService.h"

#include <algorithm>
#include <chrono>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

using namespace std::chrono_literals;

struct TemporaryProfile
{
    std::string path;

    TemporaryProfile() = default;
    explicit TemporaryProfile(std::string profilePath)
        : path(std::move(profilePath))
    {
    }
    TemporaryProfile(const TemporaryProfile&) = delete;
    TemporaryProfile& operator=(const TemporaryProfile&) = delete;
    TemporaryProfile(TemporaryProfile&& other) noexcept
        : path(std::move(other.path))
    {
        other.path.clear();
    }
    TemporaryProfile& operator=(TemporaryProfile&&) = delete;

    ~TemporaryProfile()
    {
        if (!path.empty())
        {
            std::remove(path.c_str());
        }
    }
};

struct TemporaryRecoveryProject
{
    std::string projectPath;
    std::string journalPath;
    std::string activityPath;

    TemporaryRecoveryProject()
    {
        const auto unique = std::chrono::steady_clock::now()
            .time_since_epoch().count();
        projectPath = (std::filesystem::temp_directory_path()
            / ("fidget-service-recovery-" + std::to_string(unique)
               + ".mwwcrate")).string();
        journalPath = fidget::ProjectTunerRecoveryJournalPath(projectPath);
        activityPath = fidget::ProjectActivityLogPath(projectPath);
    }

    ~TemporaryRecoveryProject()
    {
        std::remove(projectPath.c_str());
        std::remove(journalPath.c_str());
        std::remove(activityPath.c_str());
    }
};

TemporaryProfile MakeTemporaryProfile()
{
    const char* directory = std::getenv("TMPDIR");
    if (directory == nullptr || directory[0] == '\0')
    {
        directory = "/tmp";
    }
    const auto unique = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    return TemporaryProfile(
        std::string(directory) + "/fidget_service_profile_" +
        std::to_string(unique) + ".mwwscp");
}

std::vector<std::byte> EncodeWords(
    const std::vector<std::uint32_t>& words)
{
    return fidget::EncodeMvlcWordsLittleEndian(words.data(), words.size());
}

std::vector<std::byte> MakeReadRequest(
    std::uint16_t address,
    std::uint16_t reference)
{
    const auto words =
        fidget::BuildMvlcLocalRegisterReadRequest(reference, address);
    return fidget::EncodeMvlcWordsLittleEndian(words.data(), words.size());
}

std::vector<std::byte> MakeReadReply(
    std::uint16_t address,
    std::uint16_t reference,
    std::uint32_t value)
{
    return EncodeWords({
        4U,
        0U,
        (static_cast<std::uint32_t>(fidget::MvlcSuperFrameType) << 24U)
            | 3U,
        fidget::MvlcReferenceWordCommand | reference,
        fidget::MvlcReadLocalCommand | address,
        value,
    });
}

std::vector<std::byte> MakeCommandPacket(
    const std::vector<std::vector<std::uint32_t>>& frames)
{
    std::vector<std::uint32_t> words{0U, 0U};
    for (const auto& frame : frames)
    {
        words.insert(words.end(), frame.begin(), frame.end());
    }
    words[0] = static_cast<std::uint32_t>(words.size() - 2U);
    return EncodeWords(words);
}

std::vector<std::uint32_t> MakeSuperFrame(std::uint16_t reference)
{
    return {
        (static_cast<std::uint32_t>(fidget::MvlcSuperFrameType) << 24U)
            | 1U,
        fidget::MvlcReferenceWordCommand | reference,
    };
}

std::vector<std::uint32_t> MakeReadStackFrame(
    std::uint32_t stackReference,
    std::uint16_t value)
{
    return {
        (static_cast<std::uint32_t>(fidget::MvlcStackFrameType) << 24U)
            | 2U,
        stackReference,
        value,
    };
}

std::vector<std::uint32_t> MakeWriteStackFrame(
    std::uint32_t stackReference)
{
    return {
        (static_cast<std::uint32_t>(fidget::MvlcStackFrameType) << 24U)
            | 1U,
        stackReference,
    };
}

std::vector<std::byte> MakeUploadRequest(
    std::uint16_t superReference,
    std::uint32_t stackReference,
    std::uint32_t address)
{
    const auto operation = fidget::EncodeMvlcVmeReadD16Words(address);
    const auto request = fidget::BuildMvlcStackUploadRequest(
        superReference,
        stackReference,
        operation.data(),
        operation.size());
    return EncodeWords(request);
}

std::vector<std::byte> MakeExecuteRequest(std::uint16_t superReference)
{
    return EncodeWords(fidget::BuildMvlcStackExecuteRequest(superReference));
}

std::vector<std::byte> MakeWriteUploadRequest(
    std::uint16_t superReference,
    std::uint32_t stackReference,
    std::uint32_t address,
    std::uint16_t value)
{
    const auto operation = fidget::EncodeMvlcVmeWriteD16Words(address, value);
    const auto request = fidget::BuildMvlcStackUploadRequest(
        superReference,
        stackReference,
        operation.data(),
        operation.size());
    return EncodeWords(request);
}

struct CaptureReferences
{
    std::uint16_t super = 0x1800U;
    std::uint32_t stack = 0x9C100001U;
};

void QueueVmeRead(
    fidget::test::FakeCommandTransport& transport,
    CaptureReferences& references,
    std::uint32_t address,
    std::uint16_t value)
{
    transport.QueueExchange({
        MakeUploadRequest(references.super, references.stack, address),
        {fidget::test::FakeReceiveAction::Datagram(
            MakeCommandPacket({MakeSuperFrame(references.super)}))},
    });
    ++references.super;
    transport.QueueExchange({
        MakeExecuteRequest(references.super),
        {fidget::test::FakeReceiveAction::Datagram(
            MakeCommandPacket({
                MakeSuperFrame(references.super),
                MakeReadStackFrame(references.stack, value),
            }))},
    });
    ++references.super;
    ++references.stack;
}

void QueueFailedVmeRead(
    fidget::test::FakeCommandTransport& transport,
    CaptureReferences& references,
    std::uint32_t address)
{
    transport.QueueExchange({
        MakeUploadRequest(references.super, references.stack, address),
        {fidget::test::FakeReceiveAction::Datagram(
            MakeCommandPacket({MakeSuperFrame(references.super)}))},
    });
    ++references.super;
    transport.QueueExchange({
        MakeExecuteRequest(references.super),
        {fidget::test::FakeReceiveAction::Datagram(
            MakeCommandPacket({
                MakeSuperFrame(references.super),
                {
                    (static_cast<std::uint32_t>(
                         fidget::MvlcStackFrameType)
                     << 24U)
                        | (static_cast<std::uint32_t>(
                               fidget::MvlcBusErrorFlag)
                           << 20U)
                        | 2U,
                    references.stack,
                    0U,
                },
            }))},
    });
    ++references.super;
    ++references.stack;
}

void QueueVmeWrite(
    fidget::test::FakeCommandTransport& transport,
    CaptureReferences& references,
    std::uint32_t address,
    std::uint16_t value)
{
    transport.QueueExchange({
        MakeWriteUploadRequest(
            references.super, references.stack, address, value),
        {fidget::test::FakeReceiveAction::Datagram(
            MakeCommandPacket({MakeSuperFrame(references.super)}))},
    });
    ++references.super;
    transport.QueueExchange({
        MakeExecuteRequest(references.super),
        {fidget::test::FakeReceiveAction::Datagram(
            MakeCommandPacket({
                MakeSuperFrame(references.super),
                MakeWriteStackFrame(references.stack),
            }))},
    });
    ++references.super;
    ++references.stack;
}

std::uint16_t CaptureBankValue(
    std::uint16_t quad,
    std::size_t settingIndex)
{
    return static_cast<std::uint16_t>(
        1000U + quad * 100U + settingIndex);
}

void QueueCaptureGlobals(
    fidget::test::FakeCommandTransport& transport,
    CaptureReferences& references)
{
    constexpr std::uint32_t Base = 0x11000000U;
    QueueVmeRead(transport, references, Base + 0x6008U, 0x5007U);
    QueueVmeRead(transport, references, Base + 0x600EU, 0x2051U);
    QueueVmeRead(transport, references, Base + 0x6010U, 1U);
    QueueVmeRead(transport, references, Base + 0x6044U, 0x18U);
}

void QueueCaptureBank(
    fidget::test::FakeCommandTransport& transport,
    CaptureReferences& references,
    std::uint16_t quad)
{
    constexpr std::uint32_t Base = 0x11000000U;
    QueueVmeWrite(
        transport,
        references,
        Base + fidget::Fw2051ScpSelectorRegister,
        quad);
    for (std::size_t index = 0U;
         index < fidget::Fw2051ScpSettingRegistry.size(); ++index)
    {
        QueueVmeRead(
            transport,
            references,
            Base + fidget::Fw2051ScpSettingRegistry[index].registerOffset,
            CaptureBankValue(quad, index));
    }
}

void QueueRead(
    fidget::test::FakeCommandTransport& transport,
    std::uint16_t address,
    std::uint16_t reference,
    std::uint32_t value)
{
    transport.QueueExchange({
        MakeReadRequest(address, reference),
        {fidget::test::FakeReceiveAction::Datagram(
            MakeReadReply(address, reference, value))},
    });
}

void QueueCompleteConfigurationCapture(
    fidget::test::FakeCommandTransport& transport)
{
    constexpr std::uint32_t Base = 0x11000000U;
    CaptureReferences references;
    QueueRead(transport, fidget::DaqModeRegister, 4U, 0U);
    QueueCaptureGlobals(transport, references);
    QueueCaptureBank(transport, references, 0U);
    for (std::uint16_t quad = 1U;
         quad < fidget::Fw2051ScpQuadCount;
         ++quad)
    {
        QueueRead(
            transport,
            fidget::DaqModeRegister,
            static_cast<std::uint16_t>(4U + quad),
            0U);
        QueueCaptureBank(transport, references, quad);
    }
    QueueRead(transport, fidget::DaqModeRegister, 12U, 0U);
    QueueVmeWrite(
        transport,
        references,
        Base + fidget::Fw2051ScpSelectorRegister,
        0U);
}

void QueueIdleProbe(fidget::test::FakeCommandTransport& transport)
{
    QueueRead(transport, fidget::FirmwareRevisionRegister, 1U, 0x0046U);
    QueueRead(transport, fidget::DaqModeRegister, 2U, 0U);
    QueueRead(
        transport,
        fidget::HardwareIdRegister,
        3U,
        fidget::ExpectedMvlcHardwareId);
}

using AuditValues = std::array<
    std::uint16_t,
    fidget::Fw2051StartupAuditRegisterCount>;

std::size_t AuditRegisterIndex(std::uint16_t registerOffset)
{
    const auto& table = fidget::Fw2051StartupAuditRegisterTable;
    const auto found = std::find_if(
        table.begin(), table.end(),
        [registerOffset](const auto& definition) {
            return definition.registerOffset == registerOffset;
        });
    REQUIRE(found != table.end());
    return static_cast<std::size_t>(found - table.begin());
}

AuditValues MakeReadyAuditValues()
{
    AuditValues values{};
    const auto set = [&values](
                         const std::uint16_t registerOffset,
                         const std::uint16_t value) {
        values[AuditRegisterIndex(registerOffset)] = value;
    };

    set(0x6004U, 0x0011U);
    set(0x6006U, 1U);
    set(0x6008U, 0x5007U);
    set(0x600EU, 0x2051U);
    set(0x6010U, 1U);
    set(0x6012U, 0U);
    set(0x6018U, 1U);
    set(0x601AU, 1U);
    set(0x601CU, 1U);
    set(0x601EU, 1U);
    set(0x6032U, 2U);
    set(0x6036U, 3U);
    set(0x6038U, 3U);
    set(0x603AU, 0U);
    set(0x6042U, 5U);
    set(0x6044U, 0x18U);
    set(0x6046U, 0U);
    set(0x6050U, 0x3FF0U);
    set(0x6054U, 32U);
    set(0x6058U, 0x0100U);
    set(0x605CU, 1U);
    set(0x605EU, 0x0100U);
    set(0x6060U, 0U);
    set(0x6062U, 0U);
    set(0x6064U, 0U);
    set(0x6066U, 0U);
    set(0x6068U, 1U);
    set(0x606AU, 1U);
    set(0x606CU, 1U);
    set(0x6070U, 0U);
    set(0x6072U, 400U);
    set(0x6074U, 1U);
    set(0x607AU, 0U);
    set(0x607CU, 0U);
    set(0x607EU, 0U);
    set(0x6096U, 0U);
    set(0x6098U, 1U);
    return values;
}

void QueueStartupAudit(
    fidget::test::FakeCommandTransport& transport,
    const AuditValues& values)
{
    QueueRead(transport, fidget::DaqModeRegister, 4U, 0U);

    std::uint16_t superReference = 0x1600U;
    std::uint32_t stackReference = 0x9C080001U;
    for (std::size_t index = 0U;
         index < fidget::Fw2051StartupAuditRegisterTable.size(); ++index)
    {
        const auto address = 0x11000000U +
            fidget::Fw2051StartupAuditRegisterTable[index].registerOffset;
        transport.QueueExchange({
            MakeUploadRequest(superReference, stackReference, address),
            {fidget::test::FakeReceiveAction::Datagram(
                MakeCommandPacket({MakeSuperFrame(superReference)}))},
        });
        ++superReference;
        transport.QueueExchange({
            MakeExecuteRequest(superReference),
            {fidget::test::FakeReceiveAction::Datagram(
                MakeCommandPacket({
                    MakeSuperFrame(superReference),
                    MakeReadStackFrame(stackReference, values[index]),
                }))},
        });
        ++superReference;
        ++stackReference;
    }
}

std::vector<std::uint32_t> DecodeWords(
    const std::vector<std::byte>& bytes)
{
    REQUIRE(bytes.size() % sizeof(std::uint32_t) == 0U);
    std::vector<std::uint32_t> words;
    for (std::size_t offset = 0U;
         offset < bytes.size();
         offset += sizeof(std::uint32_t))
    {
        words.push_back(fidget::LoadLittleEndian32(bytes.data() + offset));
    }
    return words;
}

struct CapturedWireOperation
{
    bool write = false;
    std::uint32_t address = 0U;
    std::uint16_t value = 0U;
};

std::vector<CapturedWireOperation> DecodeCapturedWireOperations(
    const fidget::test::FakeCommandTransport& transport)
{
    std::vector<CapturedWireOperation> operations;
    for (const auto& request : transport.SentRequests())
    {
        const auto words = DecodeWords(request);
        for (std::size_t index = 0U; index < words.size(); ++index)
        {
            if (words[index] == fidget::MvlcVmeReadA32D16Command)
            {
                REQUIRE(index + 2U < words.size());
                operations.push_back({false, words[index + 2U], 0U});
            }
            else if (words[index] == fidget::MvlcVmeWriteA32D16Command)
            {
                REQUIRE(index + 4U < words.size());
                operations.push_back({
                    true,
                    words[index + 2U],
                    static_cast<std::uint16_t>(words[index + 4U]),
                });
            }
        }
    }
    return operations;
}

void CheckStartupAuditWireRequests(
    const fidget::test::FakeCommandTransport& transport)
{
    std::size_t readOperations = 0U;
    std::size_t writeOperations = 0U;
    std::vector<std::uint32_t> readAddresses;
    for (const auto& request : transport.SentRequests())
    {
        const auto words = DecodeWords(request);
        for (std::size_t index = 0U; index < words.size(); ++index)
        {
            if (words[index] == fidget::MvlcVmeReadA32D16Command)
            {
                ++readOperations;
                REQUIRE(index + 2U < words.size());
                readAddresses.push_back(words[index + 2U]);
            }
            if (words[index] == fidget::MvlcVmeWriteA32D16Command)
            {
                ++writeOperations;
            }
        }
    }

    CHECK(readOperations == 37U);
    CHECK(writeOperations == 0U);
    REQUIRE(readAddresses.size() ==
            fidget::Fw2051StartupAuditRegisterTable.size());
    for (std::size_t index = 0U; index < readAddresses.size(); ++index)
    {
        CHECK(readAddresses[index] ==
              0x11000000U +
                  fidget::Fw2051StartupAuditRegisterTable[index]
                      .registerOffset);
    }
}

fidget::CrateProject MakeProject()
{
    fidget::CrateProject project;
    project.mvlcHost = "mvlc-test";
    project.mvlcCommandPort = 32768U;
    project.streamHost = "stream-test";
    project.streamPort = 42333U;
    project.modules.push_back({
        "MDPP-32 SCP",
        0x11000000U,
        fidget::MdppBackend::Scp,
        "mdpp1_scp_profile.mwwscp",
    });
    return project;
}

fidget::TunerRecoveryRecord MakeRecoveryRecord()
{
    using namespace fidget;

    TunerRecoveryRecord record;
    record.phase = TunerRecoveryPhase::Active;
    record.host = "mvlc-test";
    record.commandPort = 32768U;
    record.mvlcHardwareId = ExpectedMvlcHardwareId;
    record.mvlcFirmwareRevision = 0x0046U;
    record.mdppBaseAddress = 0x11000000U;
    record.mdppHardwareId = 0x5007U;
    record.mdppIrqLevel = 3U;
    record.mdppOutputFormat = 0x0018U;
    record.stackTriggerRegister = 0x1104U;
    record.stackTriggerValue = 0x0042U;
    record.stackOffsetRegister = 0x1204U;
    record.stackOffsetValue = 0x0200U;
    record.ownershipTokenRegister = 0x221CU;
    record.ownershipTokenValue = 0xA55A1234U;
    return record;
}

void QueueBatchRead(
    fidget::test::FakeCommandTransport& transport,
    const std::uint16_t reference,
    const std::vector<std::uint16_t>& addresses,
    const std::vector<std::uint32_t>& values)
{
    using namespace fidget;
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
        {fidget::test::FakeReceiveAction::Datagram(
            MakeCommandPacket({frame}))},
    });
}

void QueueAlreadyCleanRecovery(
    fidget::test::FakeCommandTransport& transport,
    const fidget::TunerRecoveryRecord& record)
{
    using namespace fidget;

    QueueBatchRead(
        transport,
        0x5000U,
        {
            TunerRecoveryMvlcHardwareIdRegister,
            TunerRecoveryMvlcFirmwareRegister,
        },
        {record.mvlcHardwareId, record.mvlcFirmwareRevision});
    const auto expected = BuildTunerRecoveryFingerprintExpectation(record);
    REQUIRE(expected.success);
    auto values = std::vector<std::uint32_t>(
        expected.values.begin(), expected.values.end());
    values[0] = 0U;
    QueueBatchRead(
        transport,
        0x5001U,
        std::vector<std::uint16_t>(
            expected.addresses.begin(), expected.addresses.end()),
        values);
}

bool WaitFor(
    fidget::OwnershipService& service,
    const std::function<bool(const fidget::TunerSnapshot&)>& predicate);

fidget::Fw2051ScpConfigurationSnapshot MakeValidConfiguration()
{
    using namespace fidget;

    Fw2051ScpConfigurationSnapshot configuration;
    configuration.state = ScpConfigurationState::Complete;
    configuration.message = "service transaction test";
    configuration.baseAddress = 0x11000000U;
    configuration.hardwareId = Mdpp32HardwareId;
    configuration.firmwareRevision = Mdpp32ScpFirmwareRevisionFw2051;
    configuration.irqLevel = 1U;
    configuration.outputFormat = 0x18U;
    configuration.selectorParkedAtQuadZero = true;
    for (std::uint16_t quadIndex = 0U;
         quadIndex < Fw2051ScpQuadCount;
         ++quadIndex)
    {
        Fw2051ScpQuadConfiguration quad;
        quad.quad = quadIndex;
        quad.timingFilter = static_cast<std::uint16_t>(10U + quadIndex);
        quad.poleZero = {
            static_cast<std::uint16_t>(2000U + quadIndex * 10U),
            static_cast<std::uint16_t>(2001U + quadIndex * 10U),
            static_cast<std::uint16_t>(2002U + quadIndex * 10U),
            static_cast<std::uint16_t>(2003U + quadIndex * 10U),
        };
        quad.gain = quadIndex == 7U ? 250U : 200U;
        quad.thresholds = {
            static_cast<std::uint16_t>(2500U + quadIndex * 10U),
            static_cast<std::uint16_t>(2501U + quadIndex * 10U),
            static_cast<std::uint16_t>(2502U + quadIndex * 10U),
            static_cast<std::uint16_t>(2503U + quadIndex * 10U),
        };
        quad.shapingTime = static_cast<std::uint16_t>(160U + quadIndex);
        quad.baselineRestorer = 2U;
        quad.resetTime = 16U;
        quad.signalRiseTime = 4U;
        quad.preSamples = 50U;
        quad.totalSamples = 400U;
        quad.sampleConfiguration = quadIndex == 7U ? 3U : 0U;
        configuration.quads.push_back(quad);
    }
    return configuration;
}

std::uint16_t QueueValidConfigurationCapture(
    fidget::test::FakeCommandTransport& transport,
    const fidget::Fw2051ScpConfigurationSnapshot& configuration,
    std::uint16_t nextGateReference)
{
    using namespace fidget;

    CaptureReferences references;
    QueueRead(transport, DaqModeRegister, nextGateReference++, 0U);
    QueueVmeRead(
        transport,
        references,
        configuration.baseAddress + 0x6008U,
        configuration.hardwareId);
    QueueVmeRead(
        transport,
        references,
        configuration.baseAddress + 0x600EU,
        configuration.firmwareRevision);
    QueueVmeRead(
        transport,
        references,
        configuration.baseAddress + 0x6010U,
        configuration.irqLevel);
    QueueVmeRead(
        transport,
        references,
        configuration.baseAddress + 0x6044U,
        configuration.outputFormat);

    for (std::size_t quadIndex = 0U;
         quadIndex < configuration.quads.size();
         ++quadIndex)
    {
        if (quadIndex > 0U)
        {
            QueueRead(
                transport, DaqModeRegister, nextGateReference++, 0U);
        }
        QueueVmeWrite(
            transport,
            references,
            configuration.baseAddress + Fw2051ScpSelectorRegister,
            static_cast<std::uint16_t>(quadIndex));
        for (const auto& definition : Fw2051ScpSettingRegistry)
        {
            const auto value = Fw2051ScpQuadRegisterValue(
                configuration.quads[quadIndex],
                definition.registerOffset);
            REQUIRE(value.has_value());
            QueueVmeRead(
                transport,
                references,
                configuration.baseAddress + definition.registerOffset,
                *value);
        }
    }
    QueueRead(transport, DaqModeRegister, nextGateReference++, 0U);
    QueueVmeWrite(
        transport,
        references,
        configuration.baseAddress + Fw2051ScpSelectorRegister,
        0U);
    return nextGateReference;
}

void QueueSingleGainApply(
    fidget::test::FakeCommandTransport& transport,
    std::uint16_t nextGateReference)
{
    using namespace fidget;

    constexpr std::uint32_t Base = 0x11000000U;
    CaptureReferences references{0x3A00U, 0x9D100001U};
    QueueRead(transport, DaqModeRegister, nextGateReference++, 0U);
    QueueVmeRead(
        transport, references, Base + 0x6008U, Mdpp32HardwareId);
    QueueVmeRead(
        transport,
        references,
        Base + 0x600EU,
        Mdpp32ScpFirmwareRevisionFw2051);
    QueueRead(transport, DaqModeRegister, nextGateReference++, 0U);
    QueueVmeWrite(
        transport, references, Base + Fw2051ScpSelectorRegister, 7U);
    QueueVmeRead(transport, references, Base + 0x611AU, 250U);
    QueueRead(transport, DaqModeRegister, nextGateReference++, 0U);
    QueueVmeWrite(transport, references, Base + 0x611AU, 200U);
    QueueVmeRead(transport, references, Base + 0x611AU, 200U);
    QueueRead(transport, DaqModeRegister, nextGateReference, 0U);
    QueueVmeWrite(
        transport, references, Base + Fw2051ScpSelectorRegister, 0U);
}

void QueueBulkApply(
    fidget::test::FakeCommandTransport& transport,
    const fidget::Fw2051ScpConfigurationSnapshot& configuration,
    std::uint16_t nextGateReference)
{
    using namespace fidget;

    constexpr std::uint32_t Base = 0x11000000U;
    nextGateReference = QueueValidConfigurationCapture(
        transport, configuration, nextGateReference);
    CaptureReferences references{0x3C00U, 0x9D200001U};
    QueueRead(transport, DaqModeRegister, nextGateReference++, 0U);
    QueueVmeWrite(
        transport,
        references,
        Base + Fw2051AcquisitionControlRegister,
        Fw2051StopAcquisitionValue);
    QueueVmeRead(
        transport,
        references,
        Base + Fw2051AcquisitionControlRegister,
        Fw2051StopAcquisitionValue);
    QueueRead(transport, DaqModeRegister, nextGateReference++, 0U);
    QueueVmeWrite(
        transport, references, Base + Fw2051ScpSelectorRegister, 7U);
    QueueRead(transport, DaqModeRegister, nextGateReference++, 0U);
    QueueVmeWrite(transport, references, Base + 0x611AU, 200U);
    QueueVmeRead(transport, references, Base + 0x611AU, 200U);
    QueueRead(transport, DaqModeRegister, nextGateReference++, 0U);
    QueueVmeWrite(transport, references, Base + 0x614AU, 0U);
    QueueVmeRead(transport, references, Base + 0x614AU, 0U);
    QueueRead(transport, DaqModeRegister, nextGateReference++, 0U);
    QueueVmeWrite(
        transport, references, Base + Fw2051ScpSelectorRegister, 0U);
    QueueRead(transport, DaqModeRegister, nextGateReference, 0U);
    QueueVmeWrite(
        transport,
        references,
        Base + Fw2051FifoResetRegister,
        Fw2051ResetCommandValue);
    QueueVmeWrite(
        transport,
        references,
        Base + Fw2051ReadoutResetRegister,
        Fw2051ResetCommandValue);
}

void CaptureValidConfiguration(
    fidget::OwnershipService& service,
    fidget::test::FakeCommandTransport& transport,
    const fidget::Fw2051ScpConfigurationSnapshot& configuration)
{
    QueueValidConfigurationCapture(transport, configuration, 4U);
    service.Submit(fidget::CaptureConfigurationCommand{});
    REQUIRE(WaitFor(service, [](const fidget::TunerSnapshot& snapshot) {
        return snapshot.configurationCapture.state ==
            fidget::ScpConfigurationState::Complete;
    }));
}

TemporaryProfile SaveTestProfile(
    const fidget::Fw2051ScpConfigurationSnapshot& configuration)
{
    auto file = MakeTemporaryProfile();
    const auto saved = fidget::SaveFw2051ScpProfile(
        configuration, file.path);
    REQUIRE(saved.success);
    return file;
}

void LoadTestProfile(
    fidget::OwnershipService& service,
    const std::string& path,
    std::size_t expectedDifferences)
{
    service.Submit(fidget::LoadProfileCommand{path});
    REQUIRE(WaitFor(service, [expectedDifferences](
                                const fidget::TunerSnapshot& snapshot) {
        return snapshot.profileLoadedForTarget &&
            snapshot.configurationComparison.comparable &&
            snapshot.configurationComparison.differences.size() ==
                expectedDifferences;
    }));
}

bool WaitFor(
    fidget::OwnershipService& service,
    const std::function<bool(const fidget::TunerSnapshot&)>& predicate)
{
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate(*service.CurrentSnapshot()))
        {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return false;
}

void UseProject(fidget::OwnershipService& service)
{
    const auto projectPath = (
        std::filesystem::temp_directory_path()
        / "fidget-ownership-service-test.mwwcrate").string();
    std::remove(fidget::ProjectActivityLogPath(projectPath).c_str());
    fidget::UseCrateProjectCommand command;
    command.projectPath = projectPath;
    command.project = MakeProject();
    service.Submit(std::move(command));
    REQUIRE(WaitFor(service, [](const fidget::TunerSnapshot& snapshot) {
        return snapshot.projectActive;
    }));
    CHECK(service.CurrentSnapshot()->activeModuleProfilePath ==
          "mdpp1_scp_profile.mwwscp");
    CHECK(service.CurrentSnapshot()->recoveryJournalPath
          == projectPath + ".recovery");
    CHECK(service.CurrentSnapshot()->activityLogPath
          == projectPath + ".activity");
}

void CheckIdle(
    fidget::OwnershipService& service,
    fidget::test::FakeCommandTransport& transport)
{
    QueueIdleProbe(transport);
    service.Submit(fidget::CheckStatusCommand{});
    REQUIRE(WaitFor(service, [](const fidget::TunerSnapshot& snapshot) {
        return snapshot.ownership
            == fidget::GuidedTunerOwnershipState::Idle;
    }));
}

void ConfirmHandoffAndOpen(
    fidget::OwnershipService& service,
    fidget::test::FakeCommandTransport& transport)
{
    service.Submit(fidget::SetMvmeHandoffConfirmedCommand{true});
    REQUIRE(WaitFor(service, [](const fidget::TunerSnapshot& snapshot) {
        return snapshot.mvmeHandoffConfirmed;
    }));

    QueueIdleProbe(transport);
    service.Submit(fidget::OpenSessionCommand{});
    REQUIRE(WaitFor(service, [](const fidget::TunerSnapshot& snapshot) {
        return snapshot.ownership
            == fidget::GuidedTunerOwnershipState::SessionOpen;
    }));
}

void CheckOnlyReadRequests(
    const fidget::test::FakeCommandTransport& transport)
{
    const auto requests = transport.SentRequests();
    REQUIRE_FALSE(requests.empty());
    for (const auto& request : requests)
    {
        REQUIRE(request.size() == 4U * sizeof(std::uint32_t));
        const auto command = fidget::LoadLittleEndian32(
            request.data() + 2U * sizeof(std::uint32_t));
        CHECK((command & 0xFFFF0000U) == fidget::MvlcReadLocalCommand);
        CHECK((command & 0xFFFF0000U) != fidget::MvlcWriteLocalCommand);
    }
}

} // namespace

TEST_CASE("recovery is refused without a journal even on a busy crate")
{
    using namespace fidget;
    using namespace fidget::test;

    TemporaryRecoveryProject files;
    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(
        std::move(ownedTransport), std::chrono::hours(1));
    UseCrateProjectCommand use;
    use.projectPath = files.projectPath;
    use.project = MakeProject();
    service.Submit(std::move(use));
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.projectActive;
    }));
    CHECK(service.CurrentSnapshot()->recoveryJournalStatus
          == RecoveryJournalStatus::None);

    QueueRead(*transport, FirmwareRevisionRegister, 1U, 0x0046U);
    QueueRead(*transport, DaqModeRegister, 2U, 0x00000005U);
    service.Submit(CheckStatusCommand{});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.ownership == GuidedTunerOwnershipState::InUse;
    }));
    const auto requestsBeforeRecovery = transport->SentRequests().size();

    const auto revision = service.CurrentSnapshot()->revision;
    service.Submit(RecoverDiagnosticOrphanCommand{true});
    REQUIRE(WaitFor(service, [revision](const TunerSnapshot& snapshot) {
        return snapshot.revision > revision;
    }));
    CHECK(transport->SentRequests().size() == requestsBeforeRecovery);
    CHECK(service.CurrentSnapshot()->diagnosticRecovery.state
          == DiagnosticOrphanRecoveryState::NotRun);
}

TEST_CASE("malformed recovery journals are surfaced and retained")
{
    using namespace fidget;
    using namespace fidget::test;

    TemporaryRecoveryProject files;
    {
        std::ofstream malformed(files.journalPath, std::ios::trunc);
        malformed << "damaged recovery evidence\n";
    }
    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(
        std::move(ownedTransport), std::chrono::hours(1));
    UseCrateProjectCommand use;
    use.projectPath = files.projectPath;
    use.project = MakeProject();
    service.Submit(std::move(use));
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.recoveryJournalStatus
            == RecoveryJournalStatus::Malformed;
    }));
    const auto revision = service.CurrentSnapshot()->revision;
    service.Submit(RecoverDiagnosticOrphanCommand{true});
    REQUIRE(WaitFor(service, [revision](const TunerSnapshot& snapshot) {
        return snapshot.revision > revision;
    }));

    CHECK(transport->SentRequests().empty());
    CHECK(std::filesystem::exists(files.journalPath));
    CHECK(service.CurrentSnapshot()->recoveryRecordAvailable);
    CHECK_FALSE(service.CurrentSnapshot()->recoveryRecord.has_value());
}

TEST_CASE("recovery status bypasses idle refusal and clears an idle orphan")
{
    using namespace fidget;
    using namespace fidget::test;

    TemporaryRecoveryProject files;
    const auto record = MakeRecoveryRecord();
    REQUIRE(SaveTunerRecoveryJournal(record, files.journalPath).success);
    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(
        std::move(ownedTransport), std::chrono::hours(1));
    UseCrateProjectCommand use;
    use.projectPath = files.projectPath;
    use.project = MakeProject();
    service.Submit(std::move(use));
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.recoveryJournalStatus
            == RecoveryJournalStatus::Pending;
    }));
    CHECK(service.CurrentSnapshot()->ownership
          == GuidedTunerOwnershipState::RecoveryRequired);

    QueueRead(*transport, TunerRecoveryMvlcFirmwareRegister, 1U, 0x0046U);
    QueueRead(*transport, TunerRecoveryDaqModeRegister, 2U, 0x00000005U);
    QueueRead(
        *transport,
        TunerRecoveryMvlcHardwareIdRegister,
        3U,
        ExpectedMvlcHardwareId);
    service.Submit(RecoverDiagnosticOrphanCommand{false});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.controllerReadingsValid;
    }));
    CHECK(service.CurrentSnapshot()->mvlcDaqMode == 0x00000005U);
    CHECK(service.CurrentSnapshot()->ownership
          == GuidedTunerOwnershipState::RecoveryRequired);

    QueueAlreadyCleanRecovery(*transport, record);
    const auto activitiesBeforeRecovery =
        service.CurrentSnapshot()->activityLog.Size();
    service.Submit(RecoverDiagnosticOrphanCommand{true});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.diagnosticRecovery.state
            == DiagnosticOrphanRecoveryState::AlreadyClean;
    }));
    const auto recovered = service.CurrentSnapshot();
    CHECK_FALSE(recovered->recoveryRecordAvailable);
    CHECK(recovered->recoveryJournalStatus == RecoveryJournalStatus::None);
    CHECK(recovered->ownership == GuidedTunerOwnershipState::Disconnected);
    CHECK_FALSE(std::filesystem::exists(files.journalPath));
    REQUIRE_FALSE(recovered->diagnosticRecovery.steps.empty());
    CHECK(recovered->activityLog.Size()
          == activitiesBeforeRecovery
              + recovered->diagnosticRecovery.steps.size());
    for (std::size_t index = activitiesBeforeRecovery;
         index < recovered->activityLog.Size(); ++index)
    {
        CHECK(recovered->activityLog.Entries()[index].category
              == ActivityLogCategory::Recovery);
    }

    for (const auto& request : transport->SentRequests())
    {
        const auto words = DecodeWords(request);
        for (const auto word : words)
        {
            CHECK(word != MvlcVmeWriteA32D16Command);
            CHECK((word & 0xFFFF0000U) != MvlcWriteLocalCommand);
        }
    }
}

TEST_CASE("an idle check permits a confirmed session and release")
{
    using namespace fidget;
    using namespace fidget::test;

    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(std::move(ownedTransport), std::chrono::hours(1));
    UseProject(service);
    CheckIdle(service, *transport);

    const auto idle = service.CurrentSnapshot();
    CHECK(idle->controllerReadingsValid);
    CHECK(idle->mvlcFirmwareRevision == 0x0046U);
    CHECK(idle->mvlcHardwareId == ExpectedMvlcHardwareId);
    CHECK(idle->mvlcDaqMode == 0U);
    CHECK_FALSE(transport->IsOpen());
    CHECK(transport->OpenedHost() == "mvlc-test");
    CHECK(transport->OpenedPort() == 32768U);

    const auto beforeUnconfirmedOpen = idle->revision;
    service.Submit(OpenSessionCommand{});
    REQUIRE(WaitFor(service, [beforeUnconfirmedOpen](const TunerSnapshot& value) {
        return value.revision > beforeUnconfirmedOpen;
    }));
    CHECK(service.CurrentSnapshot()->ownership
          == GuidedTunerOwnershipState::Idle);
    CHECK_FALSE(transport->IsOpen());

    ConfirmHandoffAndOpen(service, *transport);
    CHECK(transport->IsOpen());

    service.Submit(ReleaseSessionCommand{});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.ownership
            == GuidedTunerOwnershipState::Disconnected;
    }));
    CHECK_FALSE(transport->IsOpen());
    CHECK_FALSE(service.CurrentSnapshot()->mvmeHandoffConfirmed);
    CheckOnlyReadRequests(*transport);
}

TEST_CASE("an active DAQ refuses ownership without reading the hardware ID")
{
    using namespace fidget;
    using namespace fidget::test;

    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(std::move(ownedTransport), std::chrono::hours(1));
    UseProject(service);

    QueueRead(*transport, FirmwareRevisionRegister, 1U, 0x0046U);
    QueueRead(*transport, DaqModeRegister, 2U, 0x000FU);
    service.Submit(CheckStatusCommand{});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.ownership == GuidedTunerOwnershipState::InUse;
    }));

    CHECK(service.CurrentSnapshot()->mvlcDaqMode == 0x000FU);
    CHECK(transport->SentRequests().size() == 2U);
    CHECK_FALSE(transport->IsOpen());
    CheckOnlyReadRequests(*transport);
}

TEST_CASE("a stale local-read reply is skipped before the matching reply")
{
    using namespace fidget;
    using namespace fidget::test;

    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(std::move(ownedTransport), std::chrono::hours(1));
    UseProject(service);

    transport->QueueExchange({
        MakeReadRequest(FirmwareRevisionRegister, 1U),
        {
            FakeReceiveAction::Datagram(MakeReadReply(
                FirmwareRevisionRegister, 0x7777U, 0xDEADBEEFU)),
            FakeReceiveAction::Datagram(MakeReadReply(
                FirmwareRevisionRegister, 1U, 0x0046U)),
        },
    });
    QueueRead(*transport, DaqModeRegister, 2U, 0U);
    QueueRead(
        *transport,
        HardwareIdRegister,
        3U,
        ExpectedMvlcHardwareId);

    service.Submit(CheckStatusCommand{});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.ownership == GuidedTunerOwnershipState::Idle;
    }));

    CHECK(service.CurrentSnapshot()->mvlcFirmwareRevision == 0x0046U);
    CHECK(transport->SentRequests().size() == 3U);
    CHECK(transport->ReceiveCapacities().size() == 4U);
    CheckOnlyReadRequests(*transport);
}

TEST_CASE("the idle watchdog passively releases a foreign takeover")
{
    using namespace fidget;
    using namespace fidget::test;

    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(std::move(ownedTransport), 10ms);
    UseProject(service);
    CheckIdle(service, *transport);
    ConfirmHandoffAndOpen(service, *transport);

    QueueRead(*transport, DaqModeRegister, 0x7000U, 0x000FU);
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.ownership
            == GuidedTunerOwnershipState::OwnershipLost;
    }));

    CHECK_FALSE(transport->IsOpen());
    CHECK(transport->SentRequests().size() == 7U);
    CheckOnlyReadRequests(*transport);
}

TEST_CASE("the watchdog reports uncertainty and later recovery")
{
    using namespace fidget;
    using namespace fidget::test;

    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(std::move(ownedTransport), 100ms);
    UseProject(service);
    CheckIdle(service, *transport);

    service.Submit(SetMvmeHandoffConfirmedCommand{true});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.mvmeHandoffConfirmed;
    }));

    QueueIdleProbe(*transport);
    for (int attempt = 0; attempt < 3; ++attempt)
    {
        transport->QueueExchange({
            MakeReadRequest(DaqModeRegister, 0x7000U),
            {FakeReceiveAction::Timeout()},
        });
    }
    service.Submit(OpenSessionCommand{});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return !snapshot.statusMessages.empty()
            && snapshot.statusMessages.back().summary
                == "MVLC command communication is temporarily uncertain. "
                   "No hardware operation is allowed until a later "
                   "watchdog read succeeds: No MVLC response after three "
                   "read-only attempts";
    }));

    CHECK(service.CurrentSnapshot()->ownership
          == GuidedTunerOwnershipState::SessionOpen);
    CHECK(transport->IsOpen());

    QueueRead(*transport, DaqModeRegister, 0x7001U, 0U);
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return !snapshot.statusMessages.empty()
            && snapshot.statusMessages.back().summary
                == "MVLC command communication recovered; DAQ mode is "
                   "still idle and controlled operations are available "
                   "again.";
    }));

    CHECK(service.CurrentSnapshot()->ownership
          == GuidedTunerOwnershipState::SessionOpen);
    CHECK(transport->SentRequests().size() == 10U);
    CheckOnlyReadRequests(*transport);

    service.Submit(ReleaseSessionCommand{});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.ownership
            == GuidedTunerOwnershipState::Disconnected;
    }));
}

TEST_CASE("the pre-write gate blocks when DAQ mode changed")
{
    using namespace fidget;
    using namespace fidget::test;

    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(std::move(ownedTransport), std::chrono::hours(1));
    UseProject(service);
    CheckIdle(service, *transport);
    ConfirmHandoffAndOpen(service, *transport);

    QueueRead(*transport, DaqModeRegister, 4U, 0x000FU);
    const auto gate = service.VerifyPreWriteGate("applying a parameter").get();
    CHECK_FALSE(gate.allowed);
    CHECK(gate.message
          == "A DAQ became active before applying a parameter. The tuner "
             "released its command socket without touching the MDPP, DAQ "
             "mode, or readout stacks.");
    CHECK(service.CurrentSnapshot()->ownership
          == GuidedTunerOwnershipState::OwnershipLost);
    CHECK_FALSE(transport->IsOpen());
    CHECK(transport->SentRequests().size() == 7U);
    CheckOnlyReadRequests(*transport);
}

TEST_CASE("the startup audit reads all 37 registers without a VME write")
{
    using namespace fidget;
    using namespace fidget::test;

    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(std::move(ownedTransport), std::chrono::hours(1));
    UseProject(service);
    CheckIdle(service, *transport);
    ConfirmHandoffAndOpen(service, *transport);

    const auto values = MakeReadyAuditValues();
    QueueStartupAudit(*transport, values);
    service.Submit(RunStartupAuditCommand{});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.startupAudit.state == StartupAuditState::Complete;
    }));

    const auto snapshot = service.CurrentSnapshot();
    CHECK(snapshot->ownership == GuidedTunerOwnershipState::SessionOpen);
    CHECK(snapshot->activeOperation == GuidedTunerOperation::None);
    CHECK(snapshot->startupAuditCompleteForTarget);
    CHECK(snapshot->startupAuditReady);
    CHECK(snapshot->startupAudit.baseAddress == 0x11000000U);
    CHECK(snapshot->startupAudit.hardwareId == 0x5007U);
    CHECK(snapshot->startupAudit.firmwareRevision == 0x2051U);
    CHECK(snapshot->startupAudit.registersRead == 37U);
    CHECK(snapshot->startupAudit.rows.size() == 37U);
    CHECK(snapshot->startupAudit.requiredChecks == 7U);
    CHECK(snapshot->startupAudit.requiredReady == 7U);
    CHECK(snapshot->startupAudit.blockingIssues == 0U);
    CHECK(snapshot->startupAudit.warnings == 0U);
    CHECK_FALSE(snapshot->startupAudit.vmeWritesIssued);
    CHECK(transport->SentRequests().size() == 81U);
    CheckStartupAuditWireRequests(*transport);
}

TEST_CASE("a blocked startup audit publishes the prototype counts")
{
    using namespace fidget;
    using namespace fidget::test;

    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(std::move(ownedTransport), std::chrono::hours(1));
    UseProject(service);
    CheckIdle(service, *transport);
    ConfirmHandoffAndOpen(service, *transport);

    auto values = MakeReadyAuditValues();
    values[AuditRegisterIndex(0x6044U)] = 0x08U;
    values[AuditRegisterIndex(0x603AU)] = 1U;
    values[AuditRegisterIndex(0x6070U)] = 1U;
    QueueStartupAudit(*transport, values);
    service.Submit(RunStartupAuditCommand{});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.startupAudit.state == StartupAuditState::Complete;
    }));

    const auto snapshot = service.CurrentSnapshot();
    CHECK(snapshot->startupAuditCompleteForTarget);
    CHECK_FALSE(snapshot->startupAuditReady);
    CHECK(snapshot->startupAudit.requiredChecks == 7U);
    CHECK(snapshot->startupAudit.requiredReady == 6U);
    CHECK(snapshot->startupAudit.blockingIssues == 1U);
    CHECK(snapshot->startupAudit.warnings == 2U);
    CHECK(transport->SentRequests().size() == 81U);
    CheckStartupAuditWireRequests(*transport);
}

TEST_CASE("the startup audit requires an open ownership session")
{
    using namespace fidget;
    using namespace fidget::test;

    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(std::move(ownedTransport), std::chrono::hours(1));
    UseProject(service);

    service.Submit(RunStartupAuditCommand{});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.startupAudit.state == StartupAuditState::Failed;
    }));

    CHECK(service.CurrentSnapshot()->startupAudit.message ==
          "Open a tuner session before auditing module-wide startup "
          "settings.");
    CHECK(transport->SentRequests().empty());
}

TEST_CASE("the startup audit gate stops before its first VME transaction")
{
    using namespace fidget;
    using namespace fidget::test;

    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(std::move(ownedTransport), std::chrono::hours(1));
    UseProject(service);
    CheckIdle(service, *transport);
    ConfirmHandoffAndOpen(service, *transport);

    QueueRead(*transport, DaqModeRegister, 4U, 0x000FU);
    service.Submit(RunStartupAuditCommand{});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.startupAudit.state == StartupAuditState::Failed;
    }));

    const auto snapshot = service.CurrentSnapshot();
    CHECK(snapshot->ownership == GuidedTunerOwnershipState::OwnershipLost);
    CHECK(snapshot->activeOperation == GuidedTunerOperation::None);
    CHECK_FALSE(snapshot->startupAuditCompleteForTarget);
    CHECK_FALSE(snapshot->startupAuditReady);
    CHECK(snapshot->startupAudit.message ==
          "A DAQ became active before running the startup audit. The tuner "
          "released its command socket without touching the MDPP, DAQ mode, "
          "or readout stacks.");
    CHECK(transport->SentRequests().size() == 7U);
    CheckOnlyReadRequests(*transport);
}

TEST_CASE("the service captures eight distinct banks and clears on release")
{
    using namespace fidget;
    using namespace fidget::test;

    constexpr std::uint32_t Base = 0x11000000U;
    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(std::move(ownedTransport), std::chrono::hours(1));
    UseProject(service);
    CheckIdle(service, *transport);
    ConfirmHandoffAndOpen(service, *transport);

    QueueCompleteConfigurationCapture(*transport);
    service.Submit(CaptureConfigurationCommand{});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.configurationCapture.state ==
            ScpConfigurationState::Complete;
    }));

    const auto snapshot = service.CurrentSnapshot();
    CHECK(snapshot->ownership == GuidedTunerOwnershipState::SessionOpen);
    CHECK(snapshot->activeOperation == GuidedTunerOperation::None);
    CHECK(snapshot->configurationCompleteForTarget);
    CHECK(snapshot->configurationFresh);
    CHECK(snapshot->configurationCapture.baseAddress == Base);
    CHECK(snapshot->configurationCapture.hardwareId == 0x5007U);
    CHECK(snapshot->configurationCapture.firmwareRevision == 0x2051U);
    CHECK(snapshot->configurationCapture.quads.size() == 8U);
    CHECK(snapshot->configurationCapture.selectorParkedAtQuadZero);
    CHECK(snapshot->configurationCapture.selectorWrites.size() == 9U);
    CHECK(transport->SentRequests().size() == 313U);

    for (std::uint16_t quadIndex = 0U; quadIndex < 8U; ++quadIndex)
    {
        const auto& quad = snapshot->configurationCapture.quads[quadIndex];
        CHECK(quad.quad == quadIndex);
        for (std::size_t settingIndex = 0U;
             settingIndex < Fw2051ScpSettingRegistry.size();
             ++settingIndex)
        {
            const auto value = Fw2051ScpQuadRegisterValue(
                quad,
                Fw2051ScpSettingRegistry[settingIndex].registerOffset);
            REQUIRE(value.has_value());
            CHECK(*value == CaptureBankValue(quadIndex, settingIndex));
        }
    }

    const auto operations = DecodeCapturedWireOperations(*transport);
    REQUIRE(operations.size() == 149U);
    std::vector<std::uint16_t> selectorValues;
    for (const auto& operation : operations)
    {
        if (operation.write &&
            operation.address == Base + Fw2051ScpSelectorRegister)
        {
            selectorValues.push_back(operation.value);
        }
    }
    CHECK(selectorValues ==
          std::vector<std::uint16_t>{0U, 1U, 2U, 3U, 4U,
                                     5U, 6U, 7U, 0U});

    const auto profile = MakeTemporaryProfile();
    const auto beforeSave = snapshot->revision;
    service.Submit(SaveProfileCommand{profile.path});
    REQUIRE(WaitFor(service, [beforeSave](const TunerSnapshot& value) {
        return value.revision > beforeSave;
    }));
    CHECK(service.CurrentSnapshot()->statusMessages.back().level ==
          TunerStatusLevel::Success);

    service.Submit(LoadProfileCommand{profile.path});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& value) {
        return value.profileLoaded && value.profileMatchesExactly;
    }));
    const auto compared = service.CurrentSnapshot();
    CHECK(compared->profileLoadedForTarget);
    CHECK(compared->loadedProfilePath == profile.path);
    CHECK(compared->configurationComparison.comparable);
    CHECK(compared->configurationComparison.valuesCompared == 141U);
    CHECK(compared->configurationComparison.differences.empty());

    service.Submit(ReleaseSessionCommand{});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& value) {
        return value.ownership == GuidedTunerOwnershipState::Disconnected;
    }));
    const auto released = service.CurrentSnapshot();
    CHECK_FALSE(released->configurationCompleteForTarget);
    CHECK_FALSE(released->configurationFresh);
    CHECK(released->configurationCapture.state ==
          ScpConfigurationState::NotRun);
    CHECK_FALSE(released->profileLoaded);
    CHECK_FALSE(released->profileLoadedForTarget);
    CHECK(released->loadedProfilePath.empty());
    CHECK_FALSE(released->configurationComparison.comparable);
    CHECK_FALSE(released->profileMatchesExactly);
}

TEST_CASE("foreign DAQ activity mid-capture detaches without parking")
{
    using namespace fidget;
    using namespace fidget::test;

    constexpr std::uint32_t Base = 0x11000000U;
    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(std::move(ownedTransport), std::chrono::hours(1));
    UseProject(service);
    CheckIdle(service, *transport);
    ConfirmHandoffAndOpen(service, *transport);

    CaptureReferences references;
    QueueRead(*transport, DaqModeRegister, 4U, 0U);
    QueueCaptureGlobals(*transport, references);
    QueueCaptureBank(*transport, references, 0U);
    QueueRead(*transport, DaqModeRegister, 5U, 0U);
    QueueCaptureBank(*transport, references, 1U);
    QueueRead(*transport, DaqModeRegister, 6U, 0U);
    QueueCaptureBank(*transport, references, 2U);
    QueueRead(*transport, DaqModeRegister, 7U, 0x000FU);

    service.Submit(CaptureConfigurationCommand{});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.configurationCapture.state ==
                ScpConfigurationState::Failed &&
            snapshot.ownership == GuidedTunerOwnershipState::OwnershipLost;
    }));

    const auto snapshot = service.CurrentSnapshot();
    CHECK_FALSE(transport->IsOpen());
    CHECK_FALSE(snapshot->configurationCompleteForTarget);
    CHECK_FALSE(snapshot->configurationFresh);
    CHECK(snapshot->configurationCapture.quads.size() == 3U);
    CHECK_FALSE(snapshot->configurationCapture.selectorParkedAtQuadZero);
    CHECK(snapshot->configurationCapture.message ==
          "A DAQ became active before SCP configuration bank 3. The tuner "
          "passively detached and sent no further MDPP or readout-stack "
          "write.");

    const auto operations = DecodeCapturedWireOperations(*transport);
    std::vector<std::uint16_t> selectorValues;
    for (const auto& operation : operations)
    {
        if (operation.write &&
            operation.address == Base + Fw2051ScpSelectorRegister)
        {
            selectorValues.push_back(operation.value);
        }
    }
    CHECK(selectorValues == std::vector<std::uint16_t>{0U, 1U, 2U});
}

TEST_CASE("a capture read failure parks after a final certain gate")
{
    using namespace fidget;
    using namespace fidget::test;

    constexpr std::uint32_t Base = 0x11000000U;
    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(std::move(ownedTransport), std::chrono::hours(1));
    UseProject(service);
    CheckIdle(service, *transport);
    ConfirmHandoffAndOpen(service, *transport);

    CaptureReferences references;
    QueueRead(*transport, DaqModeRegister, 4U, 0U);
    QueueCaptureGlobals(*transport, references);
    QueueCaptureBank(*transport, references, 0U);
    QueueRead(*transport, DaqModeRegister, 5U, 0U);
    QueueCaptureBank(*transport, references, 1U);
    QueueRead(*transport, DaqModeRegister, 6U, 0U);
    QueueVmeWrite(
        *transport,
        references,
        Base + Fw2051ScpSelectorRegister,
        2U);
    for (std::size_t index = 0U; index < 5U; ++index)
    {
        QueueVmeRead(
            *transport,
            references,
            Base + Fw2051ScpSettingRegistry[index].registerOffset,
            CaptureBankValue(2U, index));
    }
    QueueFailedVmeRead(
        *transport,
        references,
        Base + Fw2051ScpSettingRegistry[5].registerOffset);
    QueueRead(*transport, DaqModeRegister, 7U, 0U);
    QueueVmeWrite(
        *transport,
        references,
        Base + Fw2051ScpSelectorRegister,
        0U);

    service.Submit(CaptureConfigurationCommand{});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.configurationCapture.state ==
            ScpConfigurationState::Failed;
    }));

    const auto snapshot = service.CurrentSnapshot();
    CHECK(snapshot->ownership == GuidedTunerOwnershipState::SessionOpen);
    CHECK_FALSE(snapshot->configurationCompleteForTarget);
    CHECK_FALSE(snapshot->configurationFresh);
    CHECK(snapshot->configurationCapture.quads.size() == 2U);
    CHECK(snapshot->configurationCapture.selectorParkedAtQuadZero);
    CHECK(snapshot->configurationCapture.message.find("Quad 2") !=
          std::string::npos);

    const auto operations = DecodeCapturedWireOperations(*transport);
    REQUIRE_FALSE(operations.empty());
    CHECK(operations.back().write);
    CHECK(operations.back().address == Base + Fw2051ScpSelectorRegister);
    CHECK(operations.back().value == 0U);
}

TEST_CASE("the service applies one row and makes the capture stale")
{
    using namespace fidget;
    using namespace fidget::test;

    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(std::move(ownedTransport), std::chrono::hours(1));
    UseProject(service);
    CheckIdle(service, *transport);
    ConfirmHandoffAndOpen(service, *transport);

    const auto live = MakeValidConfiguration();
    CaptureValidConfiguration(service, *transport, live);
    auto profileConfiguration = live;
    profileConfiguration.quads[7].gain = 200U;
    const auto profile = SaveTestProfile(profileConfiguration);
    LoadTestProfile(service, profile.path, 1U);

    const auto ready = service.CurrentSnapshot();
    CHECK(ready->configurationFresh);
    CHECK(ready->profileApplicationPlan.success);
    REQUIRE(ready->profileApplicationPlan.request.steps.size() == 1U);
    CHECK(ready->profileApplicationPlan.request.steps[0].registerOffset ==
          0x611AU);

    QueueSingleGainApply(*transport, 13U);
    const auto activitiesBeforeApply = ready->activityLog.Size();
    service.Submit(ApplyProfileRowCommand{0x611AU, 7U});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.singleRepairResult.state ==
            ScpSingleRepairState::Passed;
    }));

    const auto applied = service.CurrentSnapshot();
    CHECK(applied->ownership == GuidedTunerOwnershipState::SessionOpen);
    CHECK(applied->activeOperation == GuidedTunerOperation::None);
    CHECK(applied->singleRepairResult.writeVerified);
    CHECK_FALSE(applied->singleRepairResult.rollbackAttempted);
    CHECK(applied->singleRepairResult.profileValueRetained);
    CHECK(applied->singleRepairResult.selectorParkedAtQuadZero);
    CHECK(applied->configurationCompleteForTarget);
    CHECK_FALSE(applied->configurationFresh);
    CHECK_FALSE(applied->configurationComparison.comparable);
    CHECK_FALSE(applied->profileApplicationPlan.success);
    CHECK(applied->configurationComparison.message.find(
              "Capture a fresh SCP configuration") != std::string::npos);
    REQUIRE(applied->activityLog.Size() == activitiesBeforeApply + 1U);
    const auto& activity = applied->activityLog.Entries().back();
    CHECK(activity.category == ActivityLogCategory::Apply);
    REQUIRE(activity.parameterChange.has_value());
    CHECK(activity.parameterChange->registerOffset == 0x611AU);
    CHECK(activity.parameterChange->quad == 7U);
    CHECK(activity.parameterChange->before == 250U);
    CHECK(activity.parameterChange->after == 200U);

    const auto requestCount = transport->SentRequests().size();
    const auto beforeRefusal = applied->revision;
    service.Submit(ApplyProfileRowCommand{0x611AU, 7U});
    REQUIRE(WaitFor(service, [beforeRefusal](const TunerSnapshot& snapshot) {
        return snapshot.revision > beforeRefusal;
    }));
    CHECK(transport->SentRequests().size() == requestCount);
    CHECK(service.CurrentSnapshot()->statusMessages.back().summary ==
          "Capture all eight SCP quads again before applying a profile "
          "value.");

    service.Submit(ReleaseSessionCommand{});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.ownership ==
            GuidedTunerOwnershipState::Disconnected;
    }));
    CHECK(service.CurrentSnapshot()->singleRepairResult.state ==
          ScpSingleRepairState::NotRun);
}

TEST_CASE("the service applies the planned banked differences and stales")
{
    using namespace fidget;
    using namespace fidget::test;

    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(std::move(ownedTransport), std::chrono::hours(1));
    UseProject(service);
    CheckIdle(service, *transport);
    ConfirmHandoffAndOpen(service, *transport);

    const auto live = MakeValidConfiguration();
    CaptureValidConfiguration(service, *transport, live);
    auto profileConfiguration = live;
    profileConfiguration.quads[7].gain = 200U;
    profileConfiguration.quads[7].sampleConfiguration = 0U;
    const auto profile = SaveTestProfile(profileConfiguration);
    LoadTestProfile(service, profile.path, 2U);

    const auto ready = service.CurrentSnapshot();
    REQUIRE(ready->profileApplicationPlan.success);
    REQUIRE(ready->profileApplicationPlan.request.steps.size() == 2U);
    CHECK(ready->profileApplicationPlan.request.steps[0].registerOffset ==
          0x611AU);
    CHECK(ready->profileApplicationPlan.request.steps[1].registerOffset ==
          0x614AU);

    QueueBulkApply(*transport, live, 13U);
    const auto activitiesBeforeApply = ready->activityLog.Size();
    service.Submit(ApplyAllDifferencesCommand{});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.bulkApplyResult.state == ScpBulkApplyState::Passed;
    }));

    const auto applied = service.CurrentSnapshot();
    CHECK(applied->ownership == GuidedTunerOwnershipState::SessionOpen);
    CHECK(applied->bulkApplyResult.fullPreflightMatched);
    CHECK(applied->bulkApplyResult.moduleStopVerified);
    CHECK(applied->bulkApplyResult.moduleLeftStopped);
    CHECK(applied->bulkApplyResult.writesVerified == 2U);
    CHECK(applied->bulkApplyResult.profileValuesRetained);
    CHECK(applied->bulkApplyResult.selectorParkedAtQuadZero);
    CHECK(applied->bulkApplyResult.fifoResetSent);
    CHECK(applied->bulkApplyResult.readoutResetSent);
    CHECK_FALSE(applied->configurationFresh);
    CHECK_FALSE(applied->configurationComparison.comparable);
    REQUIRE(applied->activityLog.Size() == activitiesBeforeApply + 3U);
    const auto& activities = applied->activityLog.Entries();
    REQUIRE(activities[activitiesBeforeApply].parameterChange.has_value());
    CHECK(activities[activitiesBeforeApply].parameterChange->registerOffset
          == 0x611AU);
    CHECK(activities[activitiesBeforeApply].parameterChange->before == 250U);
    CHECK(activities[activitiesBeforeApply].parameterChange->after == 200U);
    REQUIRE(activities[activitiesBeforeApply + 1U]
                .parameterChange.has_value());
    CHECK(activities[activitiesBeforeApply + 1U]
              .parameterChange->registerOffset == 0x614AU);
    CHECK(activities[activitiesBeforeApply + 1U]
              .parameterChange->before == 3U);
    CHECK(activities[activitiesBeforeApply + 1U]
              .parameterChange->after == 0U);
    CHECK(activities.back().category == ActivityLogCategory::Apply);
    CHECK_FALSE(activities.back().parameterChange.has_value());
}

TEST_CASE("the service exposes the planner reason for a global mismatch")
{
    using namespace fidget;
    using namespace fidget::test;

    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(std::move(ownedTransport), std::chrono::hours(1));
    UseProject(service);
    CheckIdle(service, *transport);
    ConfirmHandoffAndOpen(service, *transport);

    const auto live = MakeValidConfiguration();
    CaptureValidConfiguration(service, *transport, live);
    auto profileConfiguration = live;
    profileConfiguration.outputFormat = 0x10U;
    const auto profile = SaveTestProfile(profileConfiguration);
    LoadTestProfile(service, profile.path, 1U);

    const auto ready = service.CurrentSnapshot();
    CHECK_FALSE(ready->profileApplicationPlan.success);
    CHECK(ready->profileApplicationPlan.message.find(
              "global setting 'Output format' differs") !=
          std::string::npos);
    CHECK(ready->configurationFresh);

    const auto requestCount = transport->SentRequests().size();
    const auto beforeRefusal = ready->revision;
    service.Submit(ApplyAllDifferencesCommand{});
    REQUIRE(WaitFor(service, [beforeRefusal](const TunerSnapshot& snapshot) {
        return snapshot.revision > beforeRefusal;
    }));
    const auto refused = service.CurrentSnapshot();
    CHECK(refused->configurationFresh);
    CHECK(refused->bulkApplyResult.state == ScpBulkApplyState::NotRun);
    CHECK(refused->statusMessages.back().summary.find(
              "global setting 'Output format' differs") !=
          std::string::npos);
    CHECK(transport->SentRequests().size() == requestCount);
}

TEST_CASE("the service publishes the startup recipe and requires confirmation")
{
    using namespace fidget;
    using namespace fidget::test;

    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(std::move(ownedTransport), std::chrono::hours(1));
    UseProject(service);
    CheckIdle(service, *transport);
    ConfirmHandoffAndOpen(service, *transport);

    QueueStartupAudit(*transport, MakeReadyAuditValues());
    service.Submit(RunStartupAuditCommand{});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.startupAudit.state == StartupAuditState::Complete;
    }));

    const auto live = MakeValidConfiguration();
    QueueValidConfigurationCapture(*transport, live, 5U);
    service.Submit(CaptureConfigurationCommand{});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.configurationCapture.state ==
            ScpConfigurationState::Complete;
    }));

    auto profileConfiguration = live;
    profileConfiguration.quads[7].gain = 200U;
    const auto profile = SaveTestProfile(profileConfiguration);
    LoadTestProfile(service, profile.path, 1U);

    const auto reviewed = service.CurrentSnapshot();
    CHECK(reviewed->startupPlanAvailable);
    CHECK(reviewed->standaloneStartupPlan.success);
    CHECK(reviewed->standaloneStartupPlan.valuesCompared == 141U);
    CHECK(reviewed->standaloneStartupPlan.bankedDifferences == 1U);
    CHECK(reviewed->standaloneStartupPlan.bankedApplication.steps.size() ==
          1U);
    CHECK(reviewed->startupPreparationMismatches.size() == 3U);

    const auto requestCount = transport->SentRequests().size();
    const auto beforeRefusal = reviewed->revision;
    service.Submit(RunDeterministicStartupCommand{false});
    REQUIRE(WaitFor(service, [beforeRefusal](const TunerSnapshot& snapshot) {
        return snapshot.revision > beforeRefusal &&
            snapshot.deterministicStartupResult.state ==
                DeterministicStartupState::Failed;
    }));

    const auto refused = service.CurrentSnapshot();
    CHECK(refused->configurationFresh);
    CHECK(refused->startupPlanAvailable);
    CHECK_FALSE(refused->deterministicStartupPassed);
    CHECK(refused->deterministicStartupResult.message.find(
              "explicit confirmation") != std::string::npos);
    CHECK(transport->SentRequests().size() == requestCount);
}

TEST_CASE("direct acquisition refuses to bypass deterministic startup")
{
    using namespace fidget;
    using namespace fidget::test;

    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(
        std::move(ownedTransport), std::chrono::hours(1));
    UseProject(service);
    CheckIdle(service, *transport);
    ConfirmHandoffAndOpen(service, *transport);

    const auto before = service.CurrentSnapshot();
    const auto requestCount = transport->SentRequests().size();
    service.Submit(StartDiagnosticAcquisitionCommand{29U});
    REQUIRE(WaitFor(service, [revision = before->revision](
        const TunerSnapshot& snapshot) {
        return snapshot.revision > revision;
    }));

    const auto refused = service.CurrentSnapshot();
    CHECK(refused->ownership == GuidedTunerOwnershipState::SessionOpen);
    CHECK(refused->acquisition == GuidedTunerAcquisitionState::NotRun);
    CHECK_FALSE(refused->deterministicStartupPassed);
    CHECK(refused->statusMessages.back().summary
          == "Complete deterministic startup before direct acquisition.");
    CHECK(transport->SentRequests().size() == requestCount);
}

TEST_CASE("diagnostic tune commands refuse to run outside acquisition")
{
    using namespace fidget;
    using namespace fidget::test;

    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(
        std::move(ownedTransport), std::chrono::hours(1));
    UseProject(service);
    CheckIdle(service, *transport);
    ConfirmHandoffAndOpen(service, *transport);

    auto revision = service.CurrentSnapshot()->revision;
    const auto requestCount = transport->SentRequests().size();
    service.Submit(ChangeDiagnosticSourceCommand{2U});
    REQUIRE(WaitFor(service, [revision](const TunerSnapshot& snapshot) {
        return snapshot.revision > revision;
    }));
    auto refused = service.CurrentSnapshot();
    CHECK(refused->diagnosticSourceChange.state
          == DiagnosticSourceChangeState::Failed);
    CHECK(transport->SentRequests().size() == requestCount);

    revision = refused->revision;
    service.Submit(ApplyDiagnosticPreviewCommand{0x611AU, 250U});
    REQUIRE(WaitFor(service, [revision](const TunerSnapshot& snapshot) {
        return snapshot.revision > revision;
    }));
    refused = service.CurrentSnapshot();
    CHECK(refused->diagnosticParameterPreview.state
          == DiagnosticParameterPreviewState::Failed);
    CHECK(transport->SentRequests().size() == requestCount);
}

TEST_CASE("typed service operations publish one categorized completion")
{
    using namespace fidget;
    using namespace fidget::test;

    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    auto* transport = ownedTransport.get();
    OwnershipService service(
        std::move(ownedTransport), std::chrono::hours(1));
    UseProject(service);

    const auto expectCompletion = [&service](
                                      TunerCommand command,
                                      const ActivityLogCategory category) {
        const auto before = service.CurrentSnapshot();
        const auto activityCount = before->activityLog.Size();
        service.Submit(std::move(command));
        REQUIRE(WaitFor(service, [activityCount](
                                     const TunerSnapshot& snapshot) {
            return snapshot.activityLog.Size() > activityCount;
        }));
        const auto after = service.CurrentSnapshot();
        REQUIRE(after->activityLog.Size() == activityCount + 1U);
        CHECK(after->activityLog.Entries().back().category == category);
    };

    expectCompletion(
        RunStartupAuditCommand{}, ActivityLogCategory::Audit);
    expectCompletion(
        CaptureConfigurationCommand{}, ActivityLogCategory::Capture);
    expectCompletion(
        SaveProfileCommand{"unused.mwwscp"},
        ActivityLogCategory::Capture);
    expectCompletion(
        LoadProfileCommand{"missing-profile.mwwscp"},
        ActivityLogCategory::Capture);
    expectCompletion(
        ApplyProfileRowCommand{0x611AU, 7U},
        ActivityLogCategory::Apply);
    expectCompletion(
        ApplyAllDifferencesCommand{}, ActivityLogCategory::Apply);
    expectCompletion(
        RunDeterministicStartupCommand{false},
        ActivityLogCategory::Startup);
    expectCompletion(
        StartDiagnosticAcquisitionCommand{29U},
        ActivityLogCategory::Acquisition);
    expectCompletion(
        ChangeDiagnosticSourceCommand{3U}, ActivityLogCategory::Source);
    expectCompletion(
        ApplyDiagnosticPreviewCommand{0x6124U, 200U},
        ActivityLogCategory::Preview);
    expectCompletion(
        RestoreDiagnosticPreviewCommand{}, ActivityLogCategory::Preview);
    expectCompletion(
        RecoverDiagnosticOrphanCommand{true},
        ActivityLogCategory::Recovery);
    expectCompletion(
        SetMvmeHandoffConfirmedCommand{true},
        ActivityLogCategory::Session);

    QueueIdleProbe(*transport);
    expectCompletion(CheckStatusCommand{}, ActivityLogCategory::Session);
    CHECK(service.CurrentSnapshot()->ownership
          == GuidedTunerOwnershipState::Idle);

    expectCompletion(ReleaseSessionCommand{}, ActivityLogCategory::Session);
}

TEST_CASE("project activity persists while host-only activity does not")
{
    using namespace fidget;
    using namespace fidget::test;

    TemporaryRecoveryProject files;
    auto ownedTransport = std::make_unique<FakeCommandTransport>();
    OwnershipService service(
        std::move(ownedTransport), std::chrono::hours(1));

    UseCrateProjectCommand use;
    use.projectPath = files.projectPath;
    use.project = MakeProject();
    service.Submit(std::move(use));
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return snapshot.projectActive;
    }));
    REQUIRE(std::filesystem::exists(files.activityPath));
    CHECK(service.CurrentSnapshot()->activityLogPersistenceError.empty());

    const auto firstSize = std::filesystem::file_size(files.activityPath);
    service.Submit(RunStartupAuditCommand{});
    REQUIRE(WaitFor(service, [](const TunerSnapshot& snapshot) {
        return !snapshot.activityLog.Empty()
            && snapshot.activityLog.Entries().back().category
                == ActivityLogCategory::Audit;
    }));
    CHECK(std::filesystem::file_size(files.activityPath) > firstSize);

    const auto hostOnlyCandidate = (
        std::filesystem::temp_directory_path()
        / "fidget-host-only.mwwcrate.activity").string();
    std::remove(hostOnlyCandidate.c_str());
    auto hostOnlyTransport = std::make_unique<FakeCommandTransport>();
    OwnershipService hostOnly(
        std::move(hostOnlyTransport), std::chrono::hours(1));
    hostOnly.Submit(RunStartupAuditCommand{});
    REQUIRE(WaitFor(hostOnly, [](const TunerSnapshot& snapshot) {
        return !snapshot.activityLog.Empty();
    }));
    CHECK(hostOnly.CurrentSnapshot()->activityLogPath.empty());
    CHECK_FALSE(std::filesystem::exists(hostOnlyCandidate));
}
