#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "local_mvlc_emulator.h"

#include "hardware/MvlcCommandTransport.h"
#include "hardware/MvlcDataReceiver.h"

#include <string>

TEST_CASE("localhost MVLC command and data pipes complete a safe lifecycle")
{
    using namespace fidget;
    using namespace fidget::test;

    JournalPath journal;
    LocalMvlcEmulator emulator(journal.Get());
    MvlcCommandTransport commandTransport;
    MvlcDataReceiver dataReceiver;
    const auto opened = commandTransport.Open(
        "127.0.0.1", emulator.CommandPort());
    INFO(opened.error);
    REQUIRE(opened.success);

    RunDiagnosticAcquisitionLifecycle(
        commandTransport,
        dataReceiver,
        emulator,
        journal,
        [](const std::string&) {
            return ScpCaptureGateResult{
                ScpCaptureGateStatus::Allowed,
                {},
            };
        });
}
