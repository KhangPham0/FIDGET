#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "hardware/BridgeCommandTransport.h"
#include "hardware/BridgeDataReceiver.h"
#include "hardware/SshBridgeProcess.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

namespace {

std::vector<std::byte> Bytes(
    const std::initializer_list<std::uint8_t> values)
{
    std::vector<std::byte> bytes;
    bytes.reserve(values.size());
    for (const std::uint8_t value : values)
    {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

} // namespace

TEST_CASE("cat provides a complete bridge process loopback")
{
    using namespace fidget;

    auto started = SshBridgeProcess::StartProgram({"/bin/cat"});
    INFO(started.error);
    REQUIRE(started.success);
    REQUIRE(started.process != nullptr);

    auto process = std::move(started.process);
    const auto connection = process->Connection();
    BridgeCommandTransport command(connection);
    BridgeDataReceiver data(connection);

    const auto commandOpened = command.Open("mvlc-test", 32768U);
    INFO(commandOpened.error);
    REQUIRE(commandOpened.success);
    const auto dataOpened = data.Open("mvlc-test", 32769U);
    INFO(dataOpened.error);
    REQUIRE(dataOpened.success);

    const auto commandPayload = Bytes({0x10U, 0x20U, 0x30U});
    REQUIRE(command.Send(
        commandPayload.data(), commandPayload.size()).success);

    std::array<std::byte, 32U> buffer{};
    const auto commandReceived = command.Receive(
        buffer.data(), buffer.size(), 1000);
    INFO(commandReceived.error);
    REQUIRE(commandReceived.status == TransportReceiveStatus::Received);
    REQUIRE(commandReceived.bytesReceived == commandPayload.size());
    CHECK(std::equal(
        commandPayload.begin(), commandPayload.end(), buffer.begin()));

    REQUIRE(data.Send(nullptr, 0U).success);
    const auto emptyDataReceived = data.Receive(
        buffer.data(), buffer.size(), 1000);
    INFO(emptyDataReceived.error);
    REQUIRE(emptyDataReceived.status == TransportReceiveStatus::Received);
    CHECK(emptyDataReceived.bytesReceived == 0U);

    const auto dataPayload = Bytes({0xD1U, 0xD2U, 0xD3U, 0xD4U});
    REQUIRE(data.Send(dataPayload.data(), dataPayload.size()).success);
    const auto dataReceived = data.Receive(
        buffer.data(), buffer.size(), 1000);
    INFO(dataReceived.error);
    REQUIRE(dataReceived.status == TransportReceiveStatus::Received);
    REQUIRE(dataReceived.bytesReceived == dataPayload.size());
    CHECK(std::equal(
        dataPayload.begin(), dataPayload.end(), buffer.begin()));

    data.Close();
    REQUIRE(command.Send(
        commandPayload.data(), commandPayload.size()).success);
    const auto afterDataClose = command.Receive(
        buffer.data(), buffer.size(), 1000);
    INFO(afterDataClose.error);
    REQUIRE(afterDataClose.status == TransportReceiveStatus::Received);
    CHECK(afterDataClose.bytesReceived == commandPayload.size());

    command.Close();
    const auto stopStarted = std::chrono::steady_clock::now();
    process.reset();
    const auto stopDuration = std::chrono::steady_clock::now() - stopStarted;
    CHECK(stopDuration < std::chrono::seconds(2));
}

TEST_CASE("destroying a bridge process closes an active connection")
{
    using namespace fidget;

    auto started = SshBridgeProcess::StartProgram({"/bin/cat"});
    INFO(started.error);
    REQUIRE(started.success);
    auto process = std::move(started.process);
    const auto connection = process->Connection();
    BridgeCommandTransport command(connection);
    REQUIRE(command.Open("mvlc-test", 32768U).success);

    process.reset();
    const auto payload = Bytes({0x01U});
    CHECK_FALSE(command.Send(payload.data(), payload.size()).success);
}

TEST_CASE("bridge process captures child stderr")
{
    using namespace fidget;

    const std::string missingPath = "/fidget-test-no-such-input";
    auto started = SshBridgeProcess::StartProgram(
        {"/bin/cat", missingPath});
    INFO(started.error);
    REQUIRE(started.success);
    REQUIRE(started.process != nullptr);
    started.process->Stop();
    CHECK(started.process->CapturedStderr().find(missingPath)
        != std::string::npos);
    started.process->Stop();
}

TEST_CASE("bridge process terminates a child that ignores stdin EOF")
{
    using namespace fidget;

    auto started = SshBridgeProcess::StartProgram({"/bin/sleep", "30"});
    INFO(started.error);
    REQUIRE(started.success);
    REQUIRE(started.process != nullptr);

    const auto stopStarted = std::chrono::steady_clock::now();
    started.process->Stop();
    const auto stopDuration = std::chrono::steady_clock::now() - stopStarted;
    CHECK(stopDuration < std::chrono::seconds(2));
}

TEST_CASE("bridge process reports spawn failures")
{
    using namespace fidget;

    const auto started = SshBridgeProcess::StartProgram(
        {"/fidget-test-no-such-program"});
    CHECK_FALSE(started.success);
    CHECK(started.process == nullptr);
    CHECK(started.error.find("posix_spawn") != std::string::npos);
}
