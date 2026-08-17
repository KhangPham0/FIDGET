#include "core/RecoveryJournal.h"

#include <charconv>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace fidget {
namespace {

constexpr std::string_view JournalMagic = "MWW_TUNER_RECOVERY";
constexpr std::uint64_t FnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t FnvPrime = 1099511628211ULL;

void HashByte(std::uint64_t& hash, const std::uint8_t value)
{
    hash ^= value;
    hash *= FnvPrime;
}

void HashValue(std::uint64_t& hash, const std::uint32_t value)
{
    for (unsigned shift = 0U; shift < 32U; shift += 8U)
    {
        HashByte(hash, static_cast<std::uint8_t>(value >> shift));
    }
}

void HashText(std::uint64_t& hash, const std::string& value)
{
    HashValue(hash, static_cast<std::uint32_t>(value.size()));
    for (const unsigned char byte : value)
    {
        HashByte(hash, byte);
    }
}

std::uint64_t RecordChecksum(const TunerRecoveryRecord& record)
{
    std::uint64_t hash = FnvOffsetBasis;
    HashValue(hash, record.formatVersion);
    HashValue(hash, static_cast<std::uint16_t>(record.phase));
    HashText(hash, record.host);
    HashValue(hash, record.commandPort);
    HashValue(hash, record.mvlcHardwareId);
    HashValue(hash, record.mvlcFirmwareRevision);
    HashValue(hash, record.mdppBaseAddress);
    HashValue(hash, record.mdppHardwareId);
    HashValue(hash, record.mdppIrqLevel);
    HashValue(hash, record.mdppOutputFormat);
    HashValue(hash, record.stackTriggerRegister);
    HashValue(hash, record.stackTriggerValue);
    HashValue(hash, record.stackOffsetRegister);
    HashValue(hash, record.stackOffsetValue);
    HashValue(hash, record.ownershipTokenRegister);
    HashValue(hash, record.ownershipTokenValue);
    if (record.formatVersion >= 2U)
    {
        HashValue(
            hash,
            static_cast<std::uint32_t>(
                record.isolatedModuleBaseAddresses.size()));
        for (const auto baseAddress : record.isolatedModuleBaseAddresses)
        {
            HashValue(hash, baseAddress);
        }
    }
    HashValue(hash, record.previewRestoreRequired ? 1U : 0U);
    HashValue(hash, record.previewQuad);
    HashValue(hash, record.previewRegisterOffset);
    HashValue(hash, record.previewOriginalValue);
    HashValue(hash, record.previewAppliedValue);
    return hash;
}

bool ValidHost(const std::string& host)
{
    if (host.empty() || host.size() > 255U)
    {
        return false;
    }
    for (const unsigned char character : host)
    {
        if (character <= 0x20U || character == 0x7FU)
        {
            return false;
        }
    }
    return true;
}

bool ValidateRecord(const TunerRecoveryRecord& record, std::string& error)
{
    if (record.formatVersion != 1U
        && record.formatVersion != TunerRecoveryJournalFormatVersion)
    {
        error = "Unsupported tuner recovery-journal version.";
        return false;
    }
    if (record.phase != TunerRecoveryPhase::Prepared
        && record.phase != TunerRecoveryPhase::Active)
    {
        error = "Invalid tuner recovery phase.";
        return false;
    }
    if (!ValidHost(record.host) || record.commandPort == 0U)
    {
        error = "Invalid MVLC endpoint in the tuner recovery journal.";
        return false;
    }
    if (record.mvlcHardwareId != 0x5008U)
    {
        error = "The recovery journal does not identify an MVLC.";
        return false;
    }
    if (record.mdppBaseAddress == 0U
        || (record.mdppBaseAddress & 0xFFFFU) != 0U
        || (record.mdppHardwareId != 0x5007U
            && record.mdppHardwareId != 0x500CU))
    {
        error = "Invalid MDPP identity in the tuner recovery journal.";
        return false;
    }
    if (record.mdppIrqLevel < 1U || record.mdppIrqLevel > 6U
        || record.stackTriggerRegister == 0U
        || record.stackOffsetRegister == 0U
        || record.ownershipTokenRegister == 0U
        || record.ownershipTokenValue == 0U)
    {
        error =
            "Incomplete tuner ownership fingerprint in the recovery journal.";
        return false;
    }
    if (record.formatVersion == 1U
        && !record.isolatedModuleBaseAddresses.empty())
    {
        error = "A legacy recovery journal cannot contain isolated modules.";
        return false;
    }
    if (record.isolatedModuleBaseAddresses.size() > 15U)
    {
        error = "Too many isolated modules in the recovery journal.";
        return false;
    }
    for (std::size_t index = 0U;
         index < record.isolatedModuleBaseAddresses.size();
         ++index)
    {
        const auto baseAddress = record.isolatedModuleBaseAddresses[index];
        if (baseAddress == 0U || (baseAddress & 0xFFFFU) != 0U
            || baseAddress == record.mdppBaseAddress)
        {
            error =
                "Invalid isolated-module address in the recovery journal.";
            return false;
        }
        for (std::size_t previous = 0U; previous < index; ++previous)
        {
            if (record.isolatedModuleBaseAddresses[previous] == baseAddress)
            {
                error =
                    "Duplicate isolated-module address in the recovery journal.";
                return false;
            }
        }
    }
    if (record.previewRestoreRequired
        && (record.previewQuad > 7U
            || record.previewRegisterOffset == 0U))
    {
        error = "Invalid parameter-preview recovery record.";
        return false;
    }
    return true;
}

bool ExpectToken(std::istream& input,
                 const std::string_view expected,
                 std::string& error)
{
    std::string token;
    if (!(input >> token))
    {
        error = "Unexpected end of recovery journal while reading '"
            + std::string(expected) + "'.";
        return false;
    }
    if (token != expected)
    {
        error = "Expected recovery token '" + std::string(expected)
            + "', found '" + token + "'.";
        return false;
    }
    return true;
}

template<typename Integer>
bool ReadUnsigned(std::istream& input,
                  Integer& destination,
                  const char* field,
                  std::string& error)
{
    static_assert(std::is_unsigned_v<Integer>);
    std::string token;
    if (!(input >> token))
    {
        error = std::string("Missing unsigned value for ") + field + '.';
        return false;
    }

    std::uint64_t parsed = 0U;
    const auto conversion = std::from_chars(
        token.data(), token.data() + token.size(), parsed, 10);
    if (conversion.ec != std::errc{}
        || conversion.ptr != token.data() + token.size()
        || parsed > std::numeric_limits<Integer>::max())
    {
        error = std::string("Invalid unsigned value '") + token
            + "' for " + field + '.';
        return false;
    }

    destination = static_cast<Integer>(parsed);
    return true;
}

void WriteRecord(std::ostream& output, const TunerRecoveryRecord& record)
{
    output << JournalMagic << ' ' << record.formatVersion << '\n';
    output << "ENDPOINT " << record.host << ' ' << record.commandPort << '\n';
    output << "MVLC " << record.mvlcHardwareId << ' '
           << record.mvlcFirmwareRevision << '\n';
    output << "MDPP " << record.mdppBaseAddress << ' '
           << record.mdppHardwareId << ' ' << record.mdppIrqLevel << ' '
           << record.mdppOutputFormat << '\n';
    if (record.formatVersion >= 2U)
    {
        output << "ISOLATED " << record.isolatedModuleBaseAddresses.size();
        for (const auto baseAddress : record.isolatedModuleBaseAddresses)
        {
            output << ' ' << baseAddress;
        }
        output << '\n';
    }
    output << "STACK " << record.stackTriggerRegister << ' '
           << record.stackTriggerValue << ' '
           << record.stackOffsetRegister << ' '
           << record.stackOffsetValue << ' '
           << record.ownershipTokenRegister << ' '
           << record.ownershipTokenValue << '\n';
    output << "STATE " << static_cast<std::uint16_t>(record.phase) << ' '
           << (record.previewRestoreRequired ? 1U : 0U) << ' '
           << record.previewQuad << ' '
           << record.previewRegisterOffset << ' '
           << record.previewOriginalValue << ' '
           << record.previewAppliedValue << '\n';
    output << "CHECKSUM " << RecordChecksum(record) << '\n';
    output << "END\n";
}

} // namespace

TunerRecoverySerializationResult SerializeTunerRecoveryJournal(
    const TunerRecoveryRecord& record)
{
    TunerRecoverySerializationResult result;
    std::string validationError;
    if (!ValidateRecord(record, validationError))
    {
        result.message = validationError;
        return result;
    }

    std::ostringstream output;
    WriteRecord(output, record);
    result.text = output.str();
    result.success = true;
    result.message = "Serialized tuner recovery journal.";
    return result;
}

TunerRecoveryParseResult ParseTunerRecoveryJournal(const std::string& text)
{
    TunerRecoveryParseResult result;
    std::istringstream input(text);
    TunerRecoveryRecord record;
    std::string error;
    std::string magic;
    std::uint16_t phase = 0U;
    std::uint16_t previewRequired = 0U;
    std::uint16_t isolatedModuleCount = 0U;
    std::uint64_t checksum = 0U;

    if (!(input >> magic) || magic != JournalMagic
        || !ReadUnsigned(input, record.formatVersion, "format version", error)
        || !ExpectToken(input, "ENDPOINT", error)
        || !(input >> record.host)
        || !ReadUnsigned(input, record.commandPort, "command port", error)
        || !ExpectToken(input, "MVLC", error)
        || !ReadUnsigned(
            input, record.mvlcHardwareId, "MVLC hardware ID", error)
        || !ReadUnsigned(
            input, record.mvlcFirmwareRevision, "MVLC firmware", error)
        || !ExpectToken(input, "MDPP", error)
        || !ReadUnsigned(input, record.mdppBaseAddress, "MDPP base", error)
        || !ReadUnsigned(
            input, record.mdppHardwareId, "MDPP hardware ID", error)
        || !ReadUnsigned(input, record.mdppIrqLevel, "MDPP IRQ", error)
        || !ReadUnsigned(input, record.mdppOutputFormat, "MDPP output", error))
    {
        result.message = error.empty()
            ? "Malformed tuner recovery journal."
            : error;
        return result;
    }

    if (record.formatVersion != 1U
        && record.formatVersion != TunerRecoveryJournalFormatVersion)
    {
        result.message = "Unsupported tuner recovery-journal version.";
        return result;
    }

    if (record.formatVersion >= 2U)
    {
        if (!ExpectToken(input, "ISOLATED", error)
            || !ReadUnsigned(
                input,
                isolatedModuleCount,
                "isolated module count",
                error))
        {
            result.message = error;
            return result;
        }
        if (isolatedModuleCount > 15U)
        {
            result.message =
                "Too many isolated modules in the recovery journal.";
            return result;
        }

        record.isolatedModuleBaseAddresses.reserve(isolatedModuleCount);
        for (std::uint16_t index = 0U;
             index < isolatedModuleCount;
             ++index)
        {
            std::uint32_t baseAddress = 0U;
            if (!ReadUnsigned(
                    input, baseAddress, "isolated module base", error))
            {
                result.message = error;
                return result;
            }
            record.isolatedModuleBaseAddresses.push_back(baseAddress);
        }
    }

    if (!ExpectToken(input, "STACK", error)
        || !ReadUnsigned(
            input,
            record.stackTriggerRegister,
            "stack trigger register",
            error)
        || !ReadUnsigned(
            input, record.stackTriggerValue, "stack trigger value", error)
        || !ReadUnsigned(
            input,
            record.stackOffsetRegister,
            "stack offset register",
            error)
        || !ReadUnsigned(
            input, record.stackOffsetValue, "stack offset value", error)
        || !ReadUnsigned(
            input,
            record.ownershipTokenRegister,
            "ownership token register",
            error)
        || !ReadUnsigned(
            input,
            record.ownershipTokenValue,
            "ownership token value",
            error)
        || !ExpectToken(input, "STATE", error)
        || !ReadUnsigned(input, phase, "recovery phase", error)
        || !ReadUnsigned(input, previewRequired, "preview state", error)
        || !ReadUnsigned(input, record.previewQuad, "preview quad", error)
        || !ReadUnsigned(
            input, record.previewRegisterOffset, "preview register", error)
        || !ReadUnsigned(
            input, record.previewOriginalValue, "preview original", error)
        || !ReadUnsigned(
            input, record.previewAppliedValue, "preview applied", error)
        || !ExpectToken(input, "CHECKSUM", error)
        || !ReadUnsigned(input, checksum, "checksum", error)
        || !ExpectToken(input, "END", error))
    {
        result.message = error.empty()
            ? "Malformed tuner recovery journal."
            : error;
        return result;
    }

    std::string trailing;
    if (input >> trailing)
    {
        result.message = "Unexpected trailing data in tuner recovery journal.";
        return result;
    }

    record.phase = static_cast<TunerRecoveryPhase>(phase);
    if (previewRequired > 1U)
    {
        result.message = "Invalid preview state in tuner recovery journal.";
        return result;
    }
    record.previewRestoreRequired = previewRequired == 1U;

    if (!ValidateRecord(record, error))
    {
        result.message = error;
        return result;
    }
    if (checksum != RecordChecksum(record))
    {
        result.message = "Tuner recovery-journal checksum mismatch.";
        return result;
    }

    result.success = true;
    result.message = "Parsed tuner recovery journal.";
    result.record = std::move(record);
    return result;
}

TunerRecoverySaveResult SaveTunerRecoveryJournal(
    const TunerRecoveryRecord& record,
    const std::string& path)
{
    TunerRecoverySaveResult result;
    if (path.empty())
    {
        result.message = "The tuner recovery-journal path is empty.";
        return result;
    }

    const auto serialized = SerializeTunerRecoveryJournal(record);
    if (!serialized.success)
    {
        result.message = "Recovery journal not saved: "
            + serialized.message;
        return result;
    }

    const std::filesystem::path destination(path);
    std::error_code directoryError;
    if (!destination.parent_path().empty())
    {
        std::filesystem::create_directories(
            destination.parent_path(), directoryError);
    }
    if (directoryError)
    {
        result.message = "Could not create the recovery-journal directory: "
            + directoryError.message() + '.';
        return result;
    }

    auto temporary = destination;
    temporary += ".tmp";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        result.message = "Could not open temporary recovery journal '"
            + temporary.string() + "'.";
        return result;
    }

    output << serialized.text;
    output.flush();
    output.close();
    if (!output)
    {
        std::error_code removeError;
        std::filesystem::remove(temporary, removeError);
        result.message = "Writing the tuner recovery journal failed.";
        return result;
    }

    std::error_code renameError;
    std::filesystem::rename(temporary, destination, renameError);
    if (renameError)
    {
        std::error_code removeDestinationError;
        std::filesystem::remove(destination, removeDestinationError);
        renameError.clear();
        std::filesystem::rename(temporary, destination, renameError);
    }
    if (renameError)
    {
        std::error_code removeError;
        std::filesystem::remove(temporary, removeError);
        result.message = "Could not install the recovery journal: "
            + renameError.message() + '.';
        return result;
    }

    result.success = true;
    result.message = "Saved tuner recovery journal.";
    return result;
}

TunerRecoveryLoadResult LoadTunerRecoveryJournal(const std::string& path)
{
    TunerRecoveryLoadResult result;
    if (path.empty())
    {
        result.message = "The tuner recovery-journal path is empty.";
        return result;
    }

    std::ifstream input(path, std::ios::binary);
    if (!input)
    {
        std::error_code existsError;
        const bool exists = std::filesystem::exists(path, existsError);
        result.fileMissing = !exists && !existsError;
        result.message = result.fileMissing
            ? "No tuner recovery journal is present."
            : "Could not open the tuner recovery journal.";
        return result;
    }

    std::ostringstream contents;
    contents << input.rdbuf();
    if (input.bad())
    {
        result.message = "Could not read the tuner recovery journal.";
        return result;
    }

    auto parsed = ParseTunerRecoveryJournal(contents.str());
    if (!parsed.success)
    {
        result.message = std::move(parsed.message);
        return result;
    }

    result.success = true;
    result.message = "Loaded tuner recovery journal.";
    result.record = std::move(parsed.record);
    return result;
}

bool RemoveTunerRecoveryJournal(const std::string& path, std::string& error)
{
    if (path.empty())
    {
        return true;
    }

    std::error_code removeError;
    std::filesystem::remove(path, removeError);
    if (removeError)
    {
        error = removeError.message();
        return false;
    }
    return true;
}

std::string ProjectTunerRecoveryJournalPath(const std::string& projectPath)
{
    return projectPath.empty() ? std::string{} : projectPath + ".recovery";
}

} // namespace fidget
