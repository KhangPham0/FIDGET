#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "core/DeterministicStartup.h"
#include "core/ScpConfiguration.h"
#include "core/ScpRegistry.h"
#include "core/StartupPreparation.h"
#include "hardware/DeterministicStartupOperation.h"
#include "vme_test_support.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

namespace {

constexpr std::uint32_t Base = 0x11000000U;

fidget::Fw2051ScpConfigurationSnapshot MakeConfiguration()
{
    using namespace fidget;

    Fw2051ScpConfigurationSnapshot configuration;
    configuration.state = ScpConfigurationState::Complete;
    configuration.message = "deterministic startup test";
    configuration.baseAddress = Base;
    configuration.hardwareId = Mdpp32HardwareId;
    configuration.firmwareRevision = Mdpp32ScpFirmwareRevisionFw2051;
    configuration.irqLevel = 1U;
    configuration.outputFormat = 0x0018U;
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
        quad.gain = 200U;
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

fidget::StartupAuditResult MakeAudit()
{
    fidget::StartupAuditResult audit;
    audit.state = fidget::StartupAuditState::Complete;
    audit.baseAddress = Base;
    audit.hardwareId = fidget::Mdpp32HardwareId;
    audit.firmwareRevision = fidget::Mdpp32ScpFirmwareRevisionFw2051;
    return audit;
}

fidget::DeterministicStartupRequest MakeRequest()
{
    fidget::DeterministicStartupRequest request;
    request.profileLoadedForTarget = true;
    request.configurationFresh = true;
    request.startupAuditCompleteForTarget = true;
    request.confirmed = true;
    request.profile.configuration = MakeConfiguration();
    request.reviewedConfiguration = request.profile.configuration;
    request.reviewedConfiguration.quads[7].gain = 250U;
    request.startupAudit = MakeAudit();
    return request;
}

void QueueCapture(
    fidget::test::FakeCommandTransport& transport,
    const fidget::Fw2051ScpConfigurationSnapshot& configuration)
{
    using namespace fidget;
    using namespace fidget::test;

    TransactionReferences references{0x1800U, 0x9C100001U};
    QueueRead(
        transport,
        references,
        configuration.baseAddress + 0x6008U,
        configuration.hardwareId);
    QueueRead(
        transport,
        references,
        configuration.baseAddress + 0x600EU,
        configuration.firmwareRevision);
    QueueRead(
        transport,
        references,
        configuration.baseAddress + 0x6010U,
        configuration.irqLevel);
    QueueRead(
        transport,
        references,
        configuration.baseAddress + 0x6044U,
        configuration.outputFormat);
    for (std::size_t quadIndex = 0U;
         quadIndex < configuration.quads.size();
         ++quadIndex)
    {
        QueueWrite(
            transport,
            references,
            configuration.baseAddress + Fw2051ScpSelectorRegister,
            static_cast<std::uint16_t>(quadIndex));
        for (const auto& definition : Fw2051ScpSettingRegistry)
        {
            const auto value = Fw2051ScpQuadRegisterValue(
                configuration.quads[quadIndex], definition.registerOffset);
            REQUIRE(value.has_value());
            QueueRead(
                transport,
                references,
                configuration.baseAddress + definition.registerOffset,
                *value);
        }
    }
    QueueWrite(
        transport,
        references,
        configuration.baseAddress + Fw2051ScpSelectorRegister,
        0U);
}

void QueuePreparation(
    fidget::test::FakeCommandTransport& transport)
{
    using namespace fidget;
    using namespace fidget::test;

    TransactionReferences references{0x1700U, 0x9C0C0001U};
    QueueRead(transport, references, Base + 0x6008U, Mdpp32HardwareId);
    QueueRead(
        transport,
        references,
        Base + 0x600EU,
        Mdpp32ScpFirmwareRevisionFw2051);
    for (const auto& definition : Fw2051StartupPreparationRegisterTable)
    {
        QueueRead(
            transport,
            references,
            Base + definition.registerOffset,
            definition.targetValue);
    }
    QueueRead(
        transport,
        references,
        Base + Fw2051AcquisitionControlRegister,
        1U);
    QueueWrite(
        transport,
        references,
        Base + Fw2051AcquisitionControlRegister,
        0U);
    QueueRead(
        transport,
        references,
        Base + Fw2051AcquisitionControlRegister,
        0U);
    QueueWrite(
        transport,
        references,
        Base + Fw2051FifoResetRegister,
        Fw2051ResetCommandValue);
    QueueWrite(
        transport,
        references,
        Base + Fw2051ReadoutResetRegister,
        Fw2051ResetCommandValue);
    QueueRead(
        transport,
        references,
        Base + Fw2051AcquisitionControlRegister,
        0U);
    for (const auto& definition : Fw2051StartupPreparationRegisterTable)
    {
        QueueRead(
            transport,
            references,
            Base + definition.registerOffset,
            definition.targetValue);
    }
    QueueRead(
        transport,
        references,
        Base + Fw2051AcquisitionControlRegister,
        0U);
}

void QueueBulkApply(
    fidget::test::FakeCommandTransport& transport,
    const fidget::Fw2051ScpConfigurationSnapshot& live,
    const fidget::ScpProfileApplicationRequest& request)
{
    using namespace fidget;
    using namespace fidget::test;

    QueueCapture(transport, live);
    TransactionReferences references{0x3C00U, 0x9D200001U};
    QueueWrite(
        transport,
        references,
        Base + Fw2051AcquisitionControlRegister,
        0U);
    QueueRead(
        transport,
        references,
        Base + Fw2051AcquisitionControlRegister,
        0U);

    int selectedQuad = -1;
    for (const auto& step : request.steps)
    {
        if (selectedQuad != step.quad)
        {
            QueueWrite(
                transport,
                references,
                Base + Fw2051ScpSelectorRegister,
                static_cast<std::uint16_t>(step.quad));
            selectedQuad = step.quad;
        }
        QueueWrite(
            transport,
            references,
            Base + step.registerOffset,
            step.profileValue);
        QueueRead(
            transport,
            references,
            Base + step.registerOffset,
            step.profileValue);
    }
    QueueWrite(
        transport,
        references,
        Base + Fw2051ScpSelectorRegister,
        0U);
    QueueWrite(
        transport,
        references,
        Base + Fw2051FifoResetRegister,
        Fw2051ResetCommandValue);
    QueueWrite(
        transport,
        references,
        Base + Fw2051ReadoutResetRegister,
        Fw2051ResetCommandValue);
}

void QueueSuccessfulSequence(
    fidget::test::FakeCommandTransport& transport,
    const fidget::DeterministicStartupRequest& request,
    const fidget::Fw2051ScpConfigurationSnapshot& postPreparation)
{
    QueuePreparation(transport);
    QueueCapture(transport, postPreparation);
    const auto plan = fidget::PlanFw2051ScpProfileApplication(
        request.profile, postPreparation);
    REQUIRE(plan.success);
    REQUIRE_FALSE(plan.request.steps.empty());
    QueueBulkApply(transport, postPreparation, plan.request);
    QueueCapture(transport, request.profile.configuration);
}

fidget::ScpCaptureOwnershipGate AllowAllGates()
{
    return [](const std::string&) {
        return fidget::ScpCaptureGateResult{
            fidget::ScpCaptureGateStatus::Allowed, {}};
    };
}

} // namespace

TEST_CASE("deterministic startup replans from the prepared capture")
{
    using namespace fidget;
    using namespace fidget::test;

    const auto request = MakeRequest();
    auto postPreparation = request.profile.configuration;
    postPreparation.quads[7].gain = 300U;
    postPreparation.quads[7].thresholds[0] = 2600U;

    FakeCommandTransport transport;
    Open(transport);
    QueueSuccessfulSequence(transport, request, postPreparation);

    const std::atomic<bool> cancelled{false};
    const auto result = RunFw2051DeterministicStartup(
        transport, request, cancelled, AllowAllGates());

    INFO(result.message);
    CHECK(result.state == DeterministicStartupState::Passed);
    CHECK(result.startupPreparationPassed);
    CHECK(result.postPreparationCapturePassed);
    CHECK(result.reviewedPlan.bankedDifferences == 1U);
    CHECK(result.bankedWritesPlanned == 2U);
    CHECK(result.bankedApplicationNeeded);
    CHECK(result.bankedApplicationPassed);
    CHECK(result.bankedApplication.fullPreflightMatched);
    CHECK(result.finalProfileVerified);
    CHECK(result.finalComparison.valuesCompared == 141U);
    CHECK(result.finalComparison.differences.empty());
    CHECK(result.moduleLeftStopped);
    CHECK(result.message.find("Ready and stopped.") != std::string::npos);

    REQUIRE(result.postPreparationPlan.request.steps.size() == 2U);
    const auto gain = std::find_if(
        result.postPreparationPlan.request.steps.begin(),
        result.postPreparationPlan.request.steps.end(),
        [](const auto& step) { return step.registerOffset == 0x611AU; });
    REQUIRE(gain != result.postPreparationPlan.request.steps.end());
    CHECK(gain->expectedValue == 300U);
    CHECK(gain->profileValue == 200U);

    const auto operations = DecodeWireOperations(transport);
    const auto retainedGain = std::find_if(
        operations.begin(), operations.end(), [](const auto& operation) {
            return operation.write && operation.address == Base + 0x611AU &&
                operation.value == 200U;
        });
    CHECK(retainedGain != operations.end());
    const auto retainedThreshold = std::find_if(
        operations.begin(), operations.end(), [](const auto& operation) {
            return operation.write && operation.address == Base + 0x611CU &&
                operation.value == 2570U;
        });
    CHECK(retainedThreshold != operations.end());
}

TEST_CASE("deterministic startup always performs the final 141-value proof")
{
    using namespace fidget;
    using namespace fidget::test;

    auto request = MakeRequest();
    request.reviewedConfiguration = request.profile.configuration;
    const auto postPreparation = request.profile.configuration;

    FakeCommandTransport transport;
    Open(transport);
    QueuePreparation(transport);
    QueueCapture(transport, postPreparation);
    QueueCapture(transport, request.profile.configuration);

    const std::atomic<bool> cancelled{false};
    const auto result = RunFw2051DeterministicStartup(
        transport, request, cancelled, AllowAllGates());

    INFO(result.message);
    CHECK(result.state == DeterministicStartupState::Passed);
    CHECK_FALSE(result.bankedApplicationNeeded);
    CHECK(result.bankedApplicationPassed);
    CHECK(result.finalProfileVerified);
    CHECK(result.finalComparison.valuesCompared == 141U);
    CHECK(DecodeWireOperations(transport).size() == 323U);
}

TEST_CASE(
    "deterministic startup exposes planner rejections without wire traffic")
{
    using namespace fidget;
    using namespace fidget::test;

    const std::atomic<bool> cancelled{false};

    SUBCASE("wrong VME base")
    {
        auto request = MakeRequest();
        request.reviewedConfiguration.baseAddress = 0x22000000U;
        request.startupAudit.baseAddress = 0x22000000U;
        FakeCommandTransport transport;
        Open(transport);
        const auto result = RunFw2051DeterministicStartup(
            transport, request, cancelled, AllowAllGates());
        CHECK(result.state == DeterministicStartupState::Failed);
        CHECK_FALSE(result.reviewedPlan.success);
        CHECK(result.message.find("VME base") != std::string::npos);
        CHECK(transport.SentRequests().empty());
    }

    SUBCASE("unsampled saved profile")
    {
        auto request = MakeRequest();
        request.profile.configuration.outputFormat = 0x0008U;
        request.reviewedConfiguration.outputFormat = 0x0008U;
        FakeCommandTransport transport;
        Open(transport);
        const auto result = RunFw2051DeterministicStartup(
            transport, request, cancelled, AllowAllGates());
        CHECK(result.state == DeterministicStartupState::Failed);
        CHECK_FALSE(result.reviewedPlan.success);
        CHECK(result.message.find("tuner-startup profile") !=
              std::string::npos);
        CHECK(transport.SentRequests().empty());
    }

    SUBCASE("untested firmware")
    {
        auto request = MakeRequest();
        request.profile.configuration.firmwareRevision = 0x2052U;
        request.reviewedConfiguration.firmwareRevision = 0x2052U;
        request.startupAudit.firmwareRevision = 0x2052U;
        FakeCommandTransport transport;
        Open(transport);
        const auto result = RunFw2051DeterministicStartup(
            transport, request, cancelled, AllowAllGates());
        CHECK(result.state == DeterministicStartupState::Failed);
        CHECK_FALSE(result.reviewedPlan.success);
        CHECK(transport.SentRequests().empty());
    }
}

TEST_CASE("deterministic startup stops at every ownership phase boundary")
{
    using namespace fidget;
    using namespace fidget::test;

    const auto request = MakeRequest();
    auto postPreparation = request.profile.configuration;
    postPreparation.quads[7].gain = 300U;
    const std::atomic<bool> cancelled{false};

    {
        FakeCommandTransport transport;
        Open(transport);
        const auto gate = [](const std::string& operationName) {
            if (operationName ==
                "module startup preparation preflight")
            {
                return ScpCaptureGateResult{
                    ScpCaptureGateStatus::OwnershipLost,
                    "A foreign DAQ took ownership."};
            }
            return ScpCaptureGateResult{
                ScpCaptureGateStatus::Allowed, {}};
        };
        const auto result = RunFw2051DeterministicStartup(
            transport, request, cancelled, gate);
        CHECK(result.state == DeterministicStartupState::Failed);
        CHECK(result.message.find("A foreign DAQ took ownership.") !=
              std::string::npos);
        CHECK(transport.SentRequests().empty());
    }

    for (std::size_t rejectedCapture = 1U;
         rejectedCapture <= 3U;
         ++rejectedCapture)
    {
        CAPTURE(rejectedCapture);
        FakeCommandTransport transport;
        Open(transport);
        QueueSuccessfulSequence(transport, request, postPreparation);
        std::size_t captureCount = 0U;
        const auto gate = [&captureCount, rejectedCapture](
                              const std::string& operationName) {
            if (operationName == "the SCP configuration snapshot")
            {
                ++captureCount;
                if (captureCount == rejectedCapture)
                {
                    return ScpCaptureGateResult{
                        ScpCaptureGateStatus::OwnershipLost,
                        "A foreign DAQ took ownership."};
                }
            }
            return ScpCaptureGateResult{
                ScpCaptureGateStatus::Allowed, {}};
        };
        const auto result = RunFw2051DeterministicStartup(
            transport, request, cancelled, gate);
        CHECK(result.state == DeterministicStartupState::Failed);
        CHECK(result.message.find("A foreign DAQ took ownership.") !=
              std::string::npos);
        CHECK(captureCount == rejectedCapture);
    }
}

TEST_CASE("deterministic startup requires confirmation before wire traffic")
{
    using namespace fidget;
    using namespace fidget::test;

    auto request = MakeRequest();
    request.confirmed = false;
    FakeCommandTransport transport;
    Open(transport);
    const std::atomic<bool> cancelled{false};
    const auto result = RunFw2051DeterministicStartup(
        transport, request, cancelled, AllowAllGates());

    CHECK(result.state == DeterministicStartupState::Failed);
    CHECK(result.message.find("explicit confirmation") != std::string::npos);
    CHECK(transport.SentRequests().empty());
}
