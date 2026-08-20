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
