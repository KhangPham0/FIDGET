#include "core/ReadoutProtocol.h"

#include <array>

namespace fidget {
namespace {

constexpr std::uint8_t ReadoutStackId = 1U;
constexpr std::uint16_t StackMemorySegmentWords = 128U;

constexpr std::uint32_t StackStartToDataPipe = 0xF3010000U;
constexpr std::uint32_t Mblt64FifoMaximum = 0x1208FFFFU;
constexpr std::uint32_t MdppReadoutResetOffset = 0x6034U;
constexpr std::uint16_t IrqNoIackTriggerType = 2U;

} // namespace

MvlcReadoutPlanResult MakeMvlcSingleMdppReadoutPlan(
    std::uint32_t mdppBaseAddress,
    std::uint16_t irqLevel)
{
    MvlcReadoutPlanResult result;

    if ((mdppBaseAddress & 0xFFFFU) != 0U)
    {
        result.error =
            "The MDPP base must be a full 64-KiB-aligned A32 address.";
        return result;
    }

    // IRQ7 is deliberately excluded: the MVLC documentation and the MVME
    // MDPP template support IRQ1 through IRQ6 for VME readout.
    if (irqLevel < 1U || irqLevel > 6U)
    {
        result.error = "The configured MDPP IRQ level must be 1 through 6.";
        return result;
    }

    auto& plan = result.plan;
    plan.stackId = ReadoutStackId;
    plan.stackMemoryOffset = static_cast<std::uint16_t>(
        ReadoutStackId * StackMemorySegmentWords * MvlcLocalAddressIncrement);
    plan.stackOffsetRegister = static_cast<std::uint16_t>(
        MvlcStack0OffsetRegister
        + ReadoutStackId * MvlcLocalAddressIncrement);
    plan.stackTriggerRegister = static_cast<std::uint16_t>(
        MvlcStack0TriggerRegister
        + ReadoutStackId * MvlcLocalAddressIncrement);
    plan.triggerValue = static_cast<std::uint16_t>(
        (IrqNoIackTriggerType << 5U) | (irqLevel - 1U));

    const std::array<std::uint32_t, 7> stackWords{
        StackStartToDataPipe,
        Mblt64FifoMaximum,
        mdppBaseAddress,
        MvlcVmeWriteA32D16Command,
        mdppBaseAddress + MdppReadoutResetOffset,
        1U,
        MvlcStackEndCommand,
    };

    std::uint16_t localAddress = static_cast<std::uint16_t>(
        MvlcStackMemoryBegin + plan.stackMemoryOffset);
    plan.stackUploadWrites.reserve(stackWords.size());

    for (const std::uint32_t word : stackWords)
    {
        plan.stackUploadWrites.push_back({localAddress, word});
        localAddress = static_cast<std::uint16_t>(
            localAddress + MvlcLocalAddressIncrement);
    }

    result.success = true;
    return result;
}

} // namespace fidget
