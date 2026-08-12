// FIDGET: a standalone tuner for Mesytec MDPP digitizers behind an MVLC
// controller.

#include <cstdlib>
#include <cstring>
#include <memory>

#include "hardware/MvlcCommandTransport.h"
#include "hardware/MvlcDataReceiver.h"
#include "hardware/OwnershipService.h"
#include "ui/App.h"

int main(int argc, char** argv)
{
    auto transport = std::make_unique<fidget::MvlcCommandTransport>();
    auto dataReceiver = std::make_unique<fidget::MvlcDataReceiver>();
    fidget::OwnershipService tunerControl(
        std::move(transport),
        std::move(dataReceiver),
        fidget::DefaultTunerRecoveryJournalPath());
    fidget::App app(tunerControl);

    // Smoke test mode: "fidget --frames N" renders N frames and exits, so a
    // build can be verified without a person closing the window.
    if (argc == 3 && std::strcmp(argv[1], "--frames") == 0)
    {
        app.SetFrameLimit(std::strtol(argv[2], nullptr, 10));
    }

    return app.Run();
}
