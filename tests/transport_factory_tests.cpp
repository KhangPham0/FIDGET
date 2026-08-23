#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "hardware/BridgeCommandTransport.h"
#include "hardware/BridgeDataReceiver.h"
#include "hardware/MvlcCommandTransport.h"
#include "hardware/MvlcDataReceiver.h"
#include "hardware/TransportFactory.h"

#include <cstdint>
#include <string>
#include <utility>

namespace {

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

} // namespace

TEST_CASE("the transport factory selects direct UDP transports")
{
    using namespace fidget;

    MvlcTransportFactory factory;
    auto created = factory.Create(MakeProject());
    INFO(created.error);
    REQUIRE(created.session != nullptr);
    CHECK(dynamic_cast<MvlcCommandTransport*>(
              created.session->CommandTransport())
          != nullptr);
    CHECK(dynamic_cast<MvlcDataReceiver*>(
              created.session->DataReceiver())
          != nullptr);
    CHECK(created.session->CapturedBridgeStderr().empty());
}

TEST_CASE("a direct endpoint request is independent of a crate project")
{
    using namespace fidget;

    MvlcTransportFactory factory;
    const TransportEndpointRequest request =
        DirectEthernetEndpointRequest{"mvlc-test", 32768U};
    auto created = factory.Create(request);
    INFO(created.error);
    REQUIRE(created.session != nullptr);
    CHECK(dynamic_cast<MvlcCommandTransport*>(
              created.session->CommandTransport())
          != nullptr);
    CHECK(dynamic_cast<MvlcDataReceiver*>(
              created.session->DataReceiver())
          != nullptr);
    CHECK(created.session->CapturedBridgeStderr().empty());
}

TEST_CASE("the transport factory selects one shared SSH bridge process")
{
    using namespace fidget;

    std::string destination;
    std::string command;
    std::string host;
    std::uint16_t port = 0U;
    MvlcTransportFactory factory(
        [&](const std::string& nextDestination,
            const std::string& nextCommand,
            const std::string& nextHost,
            const std::uint16_t nextPort) {
            destination = nextDestination;
            command = nextCommand;
            host = nextHost;
            port = nextPort;
            return SshBridgeProcess::StartProgram({"/bin/cat"});
        });
    auto project = MakeProject();
    project.endpointKind = CrateProjectEndpointKind::SshBridge;
    project.sshDestination = "daq-through-bastion";
    project.remoteBridgeCommand = "/opt/fidget/bin/fidget_bridge";

    auto created = factory.Create(project);
    INFO(created.error);
    REQUIRE(created.session != nullptr);
    CHECK(destination == project.sshDestination);
    CHECK(command == project.remoteBridgeCommand);
    CHECK(host == project.mvlcHost);
    CHECK(port == project.mvlcCommandPort);
    CHECK(dynamic_cast<BridgeCommandTransport*>(
              created.session->CommandTransport())
          != nullptr);
    CHECK(dynamic_cast<BridgeDataReceiver*>(
              created.session->DataReceiver())
          != nullptr);
    created.session->Close();
    CHECK(created.session->CapturedBridgeStderr().empty());
}

TEST_CASE("an SSH endpoint request carries only bridge routing fields")
{
    using namespace fidget;

    std::string destination;
    std::string command;
    std::string host;
    std::uint16_t port = 0U;
    MvlcTransportFactory factory(
        [&](const std::string& nextDestination,
            const std::string& nextCommand,
            const std::string& nextHost,
            const std::uint16_t nextPort) {
            destination = nextDestination;
            command = nextCommand;
            host = nextHost;
            port = nextPort;
            return SshBridgeProcess::StartProgram({"/bin/cat"});
        });
    const TransportEndpointRequest request = SshBridgeEndpointRequest{
        "mvlc-test",
        32768U,
        "daq-through-bastion",
        "fidget_bridge",
    };

    auto created = factory.Create(request);
    INFO(created.error);
    REQUIRE(created.session != nullptr);
    CHECK(destination == "daq-through-bastion");
    CHECK(command == "fidget_bridge");
    CHECK(host == "mvlc-test");
    CHECK(port == 32768U);
    CHECK(dynamic_cast<BridgeCommandTransport*>(
              created.session->CommandTransport())
          != nullptr);
    CHECK(dynamic_cast<BridgeDataReceiver*>(
              created.session->DataReceiver())
          != nullptr);
    created.session->Close();
    CHECK(created.session->CapturedBridgeStderr().empty());
}

TEST_CASE("endpoint requests fail closed before transport creation")
{
    using namespace fidget;

    bool bridgeStarted = false;
    MvlcTransportFactory factory(
        [&](const std::string&,
            const std::string&,
            const std::string&,
            const std::uint16_t) {
            bridgeStarted = true;
            return SshBridgeProcessStartResult{};
        });

    auto invalidDirect = factory.Create(TransportEndpointRequest{
        DirectEthernetEndpointRequest{"", 32768U},
    });
    CHECK(invalidDirect.session == nullptr);
    CHECK_FALSE(invalidDirect.error.empty());

    auto invalidBridge = factory.Create(TransportEndpointRequest{
        SshBridgeEndpointRequest{
            "mvlc-test",
            32768U,
            "",
            "fidget_bridge",
        },
    });
    CHECK(invalidBridge.session == nullptr);
    CHECK_FALSE(invalidBridge.error.empty());
    CHECK_FALSE(bridgeStarted);
}
