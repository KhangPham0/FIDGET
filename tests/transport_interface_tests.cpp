#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "fake_command_transport.h"

#include <array>
#include <cstddef>
#include <vector>

TEST_CASE("the scripted command transport matches requests and replies")
{
    using namespace fidget;
    using namespace fidget::test;

    FakeCommandTransport transport;
    const std::vector<std::byte> request{
        std::byte{0x01U}, std::byte{0x02U}};
    const std::vector<std::byte> reply{
        std::byte{0x03U}, std::byte{0x04U}, std::byte{0x05U}};
    transport.QueueExchange({
        request,
        {FakeReceiveAction::Datagram(reply)},
    });

    CHECK(transport.Open("mvlc-test", 32768U).success);
    CHECK(transport.OpenedHost() == "mvlc-test");
    CHECK(transport.OpenedPort() == 32768U);
    CHECK(transport.Send(request.data(), request.size()).success);

    std::array<std::byte, 16> buffer{};
    const auto received = transport.Receive(buffer.data(), buffer.size(), 50);
    REQUIRE(received.status == TransportReceiveStatus::Received);
    REQUIRE(received.bytesReceived == reply.size());
    CHECK(std::equal(reply.begin(), reply.end(), buffer.begin()));
    REQUIRE(transport.SentRequests().size() == 1U);
    CHECK(transport.SentRequests().front() == request);
    REQUIRE(transport.ReceiveCapacities().size() == 1U);
    CHECK(transport.ReceiveCapacities().front() == buffer.size());

    const auto timeout = transport.Receive(buffer.data(), buffer.size(), 50);
    CHECK(timeout.status == TransportReceiveStatus::Timeout);
    CHECK(timeout.error == "receive: MVLC response timed out");

    transport.Close();
    CHECK_FALSE(transport.IsOpen());
}
