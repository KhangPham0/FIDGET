#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "local_mvlc_emulator.h"

#include "hardware/BridgeCommandTransport.h"
#include "hardware/BridgeDataReceiver.h"
#include "hardware/OwnershipService.h"
#include "hardware/SshBridgeProcess.h"
#include "hardware/VmeTransaction.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#ifndef FIDGET_BRIDGE_BINARY_PATH
#error "FIDGET_BRIDGE_BINARY_PATH must name the fidget_bridge test binary"
#endif

namespace {

fidget::SshBridgeProcessStartResult StartBridge(
    const fidget::test::LocalMvlcEmulator& emulator)
{
    return fidget::SshBridgeProcess::StartProgram({
        FIDGET_BRIDGE_BINARY_PATH,
        "127.0.0.1",
        std::to_string(emulator.CommandPort()),
    });
}

void CheckControllerStatus(fidget::ICommandTransport& transport)
{
    using namespace fidget;

    constexpr std::array<std::uint16_t, 3> addresses{{
        FirmwareRevisionRegister,
        DaqModeRegister,
        HardwareIdRegister,
    }};
    constexpr std::array<std::uint32_t, 3> expected{{
        0x0046U,
        0U,
        ExpectedMvlcHardwareId,
    }};
    const std::atomic<bool> cancelled{false};
    std::uint16_t nextReference = 1U;
    for (std::size_t index = 0U; index < addresses.size(); ++index)
    {
        const auto read = ReadLocalRegisters(
            transport,
            &addresses[index],
            1U,
            nextReference,
            cancelled);
        INFO(read.error);
        REQUIRE(read.success);
        REQUIRE(read.values.size() == 1U);
        CHECK(read.values.front() == expected[index]);
    }
}

} // namespace

TEST_CASE("the real bridge binary completes a diagnostic lifecycle")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    LocalMvlcEmulator emulator(journal.Get());

    {
        auto started = StartBridge(emulator);
        INFO(started.error);
        REQUIRE(started.success);
        REQUIRE(started.process != nullptr);

        auto process = std::move(started.process);
        const auto connection = process->Connection();
        BridgeCommandTransport commandTransport(connection);
        const auto opened = commandTransport.Open(
            "127.0.0.1", emulator.CommandPort());
        INFO(opened.error);
        REQUIRE(opened.success);

        CheckControllerStatus(commandTransport);
        commandTransport.Close();
        process->Stop();
        CHECK(process->CapturedStderr().empty());
    }
    CHECK_FALSE(emulator.FirstStackWriteSeen());
    CHECK_FALSE(std::filesystem::exists(journal.Get()));

    {
        auto started = StartBridge(emulator);
        INFO(started.error);
        REQUIRE(started.success);
        REQUIRE(started.process != nullptr);

        auto process = std::move(started.process);
        const auto connection = process->Connection();
        BridgeCommandTransport commandTransport(connection);
        BridgeDataReceiver dataReceiver(connection);
        const auto opened = commandTransport.Open(
            "127.0.0.1", emulator.CommandPort());
        INFO(opened.error);
        REQUIRE(opened.success);

        CheckControllerStatus(commandTransport);
        const std::atomic<bool> cancelled{false};
        std::uint16_t nextGateReference = 0x6800U;
        RunDiagnosticAcquisitionLifecycle(
            commandTransport,
            dataReceiver,
            emulator,
            journal,
            [&](const std::string& operationName) {
                constexpr std::uint16_t address = DaqModeRegister;
                const auto daq = ReadLocalRegisters(
                    commandTransport,
                    &address,
                    1U,
                    nextGateReference,
                    cancelled);
                if (!daq.success || daq.values.size() != 1U)
                {
                    return ScpCaptureGateResult{
                        ScpCaptureGateStatus::CommunicationUnavailable,
                        operationName + ": " + daq.error,
                    };
                }
                if (daq.values.front() != 0U)
                {
                    return ScpCaptureGateResult{
                        ScpCaptureGateStatus::OwnershipLost,
                        operationName + ": MVLC DAQ mode is active",
                    };
                }
                return ScpCaptureGateResult{
                    ScpCaptureGateStatus::Allowed,
                    {},
                };
            });

        commandTransport.Close();
        process->Stop();
        CHECK(process->CapturedStderr().empty());
    }
}
