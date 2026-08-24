#include "core/RecoveryJournal.h"

#include "core/RecoveryJournalV5.h"

#include <charconv>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <ostream>
#include <sstream>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace fidget {
namespace {

constexpr std::string_view JournalMagic = "MWW_TUNER_RECOVERY";
constexpr std::uint64_t FnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t FnvPrime = 1099511628211ULL;
constexpr std::size_t MaximumAtomicJournalCollisions = 64U;

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
    if (record.formatVersion >= 3U)
    {
        HashValue(hash, record.sourceRestoreRequired ? 1U : 0U);
        HashValue(hash, record.sourceQuad);
        HashValue(hash, record.sourceOriginalConfiguration);
    }
    if (record.formatVersion >= 4U)
    {
        HashValue(hash, record.sourceAppliedConfiguration);
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
    if (record.formatVersion < 1U
        || record.formatVersion > TunerRecoveryJournalFormatVersion)
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
    if (record.sourceRestoreRequired && record.sourceQuad > 7U)
    {
        error = "Invalid waveform-source recovery record.";
        return false;
    }
    if (record.formatVersion < 4U
        && record.sourceAppliedConfigurationAvailable)
    {
        error =
            "A legacy recovery journal cannot contain an applied waveform-source configuration.";
        return false;
    }
    if (record.formatVersion >= 4U && record.sourceRestoreRequired
        && !record.sourceAppliedConfigurationAvailable)
    {
        error =
            "The waveform-source recovery record lacks its applied configuration.";
        return false;
    }
    if (record.sourceAppliedConfigurationAvailable
        && (record.sourceAppliedConfiguration
                == record.sourceOriginalConfiguration
            || ((record.sourceAppliedConfiguration
                     ^ record.sourceOriginalConfiguration)
                & 0xFFFCU)
                != 0U))
    {
        error =
            "The applied waveform-source configuration is inconsistent with its original.";
        return false;
    }
    if (!record.sourceRestoreRequired
        && record.sourceAppliedConfigurationAvailable)
    {
        error =
            "An inactive waveform-source recovery record cannot contain an applied configuration.";
        return false;
    }
    return true;
}

enum class AtomicJournalInstallMode
{
    Upsert,
    Exclusive,
    ReplaceExisting,
};

void RemoveAtomicWorkspace(const std::filesystem::path& path)
{
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
}

bool SyncPath(const std::filesystem::path& path, std::string& error)
{
    const int descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor < 0)
    {
        error = "Could not open durable recovery-journal state: "
            + std::string(std::strerror(errno)) + '.';
        return false;
    }
    const int syncResult = ::fsync(descriptor);
    const int syncError = errno;
    const int closeResult = ::close(descriptor);
    const int closeError = errno;
    if (syncResult != 0)
    {
        error = "Could not make the recovery journal durable: "
            + std::string(std::strerror(syncError)) + '.';
        return false;
    }
    if (closeResult != 0)
    {
        error = "Could not close durable recovery-journal state: "
            + std::string(std::strerror(closeError)) + '.';
        return false;
    }
    return true;
}

bool SynchronizeJournalPath(
    const std::filesystem::path& path,
    const TunerRecoveryJournalSaveRuntime& runtime,
    std::string& error)
{
    if (runtime.synchronize)
    {
        try
        {
            return runtime.synchronize(path.string(), error);
        }
        catch (...)
        {
            error = "The injected recovery-journal sync failed.";
            return false;
        }
    }
    return SyncPath(path, error);
}

bool ReplaceJournalPath(
    const std::filesystem::path& temporary,
    const std::filesystem::path& destination,
    const TunerRecoveryJournalSaveRuntime& runtime,
    std::string& error)
{
    if (runtime.replace)
    {
        try
        {
            return runtime.replace(
                temporary.string(), destination.string(), error);
        }
        catch (...)
        {
            error = "The injected recovery-journal replacement failed";
            return false;
        }
    }

    std::error_code renameError;
    std::filesystem::rename(temporary, destination, renameError);
    if (renameError)
    {
        error = renameError.message();
        return false;
    }
    return true;
}

bool JournalPathsShareFilesystem(
    const std::filesystem::path& stagingDirectory,
    const std::filesystem::path& destinationAuthority,
    const TunerRecoveryJournalSaveRuntime& runtime,
    std::string& error)
{
    if (runtime.sameFilesystem)
    {
        try
        {
            return runtime.sameFilesystem(
                stagingDirectory.string(),
                destinationAuthority.string(), error);
        }
        catch (...)
        {
            error = "The injected recovery-journal filesystem check failed.";
            return false;
        }
    }

    struct stat stagingStatus{};
    struct stat destinationStatus{};
    if (::stat(stagingDirectory.c_str(), &stagingStatus) != 0)
    {
        error = "Could not inspect the recovery-journal staging filesystem: "
            + std::string(std::strerror(errno)) + '.';
        return false;
    }
    if (::stat(destinationAuthority.c_str(), &destinationStatus) != 0)
    {
        error = "Could not inspect the recovery-journal destination "
            "filesystem: " + std::string(std::strerror(errno)) + '.';
        return false;
    }
    if (stagingStatus.st_dev != destinationStatus.st_dev)
    {
        error = "The recovery-journal staging workspace is not on the "
            "destination filesystem.";
        return false;
    }
    return true;
}

TunerRecoverySaveResult InstallSerializedJournalAtomically(
    const std::string& text,
    const std::filesystem::path& requestedDestination,
    const AtomicJournalInstallMode mode,
    const TunerRecoveryJournalSaveRuntime& runtime)
{
    TunerRecoverySaveResult result;
    std::error_code absoluteError;
    const auto destination = std::filesystem::absolute(
        requestedDestination, absoluteError).lexically_normal();
    if (absoluteError || destination.filename().empty())
    {
        result.message = "The recovery-journal destination is invalid.";
        return result;
    }

    const auto recoveryDirectory = destination.parent_path();
    std::error_code directoryError;
    const auto directoryStatus = std::filesystem::symlink_status(
        recoveryDirectory, directoryError);
    if (directoryError
        || directoryStatus.type() != std::filesystem::file_type::directory)
    {
        result.message =
            "The recovery-journal directory is not a pre-existing, durably "
            "prepared plain directory.";
        return result;
    }

    std::error_code destinationError;
    const auto destinationStatus = std::filesystem::symlink_status(
        destination, destinationError);
    if (destinationError
        && destinationError != std::errc::no_such_file_or_directory)
    {
        result.message = "Could not inspect the recovery-journal destination: "
            + destinationError.message() + '.';
        return result;
    }
    const bool destinationExists = !destinationError
        && destinationStatus.type() != std::filesystem::file_type::not_found;
    if (destinationExists)
    {
        if (destinationStatus.type()
            != std::filesystem::file_type::regular)
        {
            result.message =
                "The recovery-journal destination is not a plain file.";
            return result;
        }
    }

    if (mode == AtomicJournalInstallMode::Exclusive && destinationExists)
    {
        result.destinationAlreadyExists = true;
        result.message =
            "A recovery journal already exists at the exclusive destination.";
        return result;
    }
    if (mode == AtomicJournalInstallMode::ReplaceExisting
        && !destinationExists)
    {
        result.message = "The Prepared recovery journal is not a plain file.";
        return result;
    }

    std::filesystem::path workspace;
    std::string workspaceError;
    // A staging workspace is always created directly in the recovery
    // directory. The reserved prefix cannot be mistaken for a journal and
    // gives phase-D discovery one explicit ignored entry class. Direct-child
    // creation plus the device check below makes cross-device rename
    // impossible without relying on a fallback outside this directory.
    for (std::size_t attempt = 0U;
         attempt < MaximumAtomicJournalCollisions;
         ++attempt)
    {
        auto candidate = recoveryDirectory
            / (std::string(TunerRecoveryJournalStagingDirectoryPrefix)
               + (attempt == 0U
                      ? std::string{}
                      : "." + std::to_string(attempt)));
        std::error_code createError;
        if (std::filesystem::create_directory(candidate, createError))
        {
            workspace = std::move(candidate);
            break;
        }
        if (!createError)
            continue;

        std::error_code existsError;
        const bool collision = std::filesystem::exists(
            candidate, existsError);
        if (collision && !existsError)
            continue;
        workspaceError = createError.message();
        break;
    }
    if (workspace.empty())
    {
        result.message = workspaceError.empty()
            ? "Could not allocate an atomic recovery-journal workspace."
            : "Could not create an atomic recovery-journal workspace: "
                + workspaceError + '.';
        return result;
    }

    std::string filesystemError;
    const auto& destinationAuthority = destinationExists
        ? destination
        : recoveryDirectory;
    if (!JournalPathsShareFilesystem(
            workspace, destinationAuthority, runtime, filesystemError))
    {
        RemoveAtomicWorkspace(workspace);
        result.message = filesystemError.empty()
            ? "Could not prove same-filesystem recovery-journal staging."
            : std::move(filesystemError);
        return result;
    }

    const auto temporary = workspace / "journal";
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        RemoveAtomicWorkspace(workspace);
        result.message = "Could not open the temporary recovery journal.";
        return result;
    }
    bool writeSucceeded = false;
    try
    {
        writeSucceeded = runtime.writer
            ? runtime.writer(output, text)
            : static_cast<bool>(output.write(
                  text.data(), static_cast<std::streamsize>(text.size())));
    }
    catch (...)
    {
        writeSucceeded = false;
    }
    output.flush();
    output.close();
    if (!writeSucceeded || !output)
    {
        RemoveAtomicWorkspace(workspace);
        result.message = "Writing the temporary recovery journal failed.";
        return result;
    }

    std::string syncError;
    if (!SynchronizeJournalPath(temporary, runtime, syncError))
    {
        RemoveAtomicWorkspace(workspace);
        result.message = syncError.empty()
            ? "Could not make the temporary recovery journal durable."
            : std::move(syncError);
        return result;
    }

    std::string installError;
    bool installed = false;
    if (mode == AtomicJournalInstallMode::Exclusive)
    {
        std::error_code linkError;
        std::filesystem::create_hard_link(temporary, destination, linkError);
        installed = !linkError;
        if (linkError)
        {
            result.destinationAlreadyExists =
                linkError == std::errc::file_exists;
            installError = linkError.message();
        }
    }
    else
    {
        installed = ReplaceJournalPath(
            temporary, destination, runtime, installError);
    }
    if (!installed)
    {
        RemoveAtomicWorkspace(workspace);
        result.message = result.destinationAlreadyExists
            ? "A recovery journal already exists at the exclusive destination."
            : "Could not atomically install the recovery journal: "
                + (installError.empty()
                       ? std::string("installation failed.")
                       : installError + '.');
        return result;
    }
    result.destinationInstalled = true;

    std::string directorySyncError;
    const bool directorySynced = SynchronizeJournalPath(
        recoveryDirectory, runtime, directorySyncError);
    RemoveAtomicWorkspace(workspace);
    if (!directorySynced)
    {
        result.message = directorySyncError.empty()
            ? "Could not make the installed recovery journal durable."
            : std::move(directorySyncError);
        return result;
    }

    result.success = true;
    result.message = mode == AtomicJournalInstallMode::Exclusive
        ? "Created the tuner recovery journal exclusively and durably."
        : mode == AtomicJournalInstallMode::ReplaceExisting
            ? "Promoted the tuner recovery journal atomically and durably."
            : "Saved tuner recovery journal durably.";
    return result;
}

bool SameEndpoint(
    const ControllerEndpointRequest& left,
    const ControllerEndpointRequest& right)
{
    return left == right;
}

bool SameIdentity(
    const TunerRecoveryV5IdentityEvidence& left,
    const TunerRecoveryV5IdentityEvidence& right)
{
    return left.mvlcHardwareId == right.mvlcHardwareId
        && left.mvlcFirmwareRevision == right.mvlcFirmwareRevision
        && left.targetBaseAddress == right.targetBaseAddress
        && left.targetHardwareId == right.targetHardwareId
        && left.targetFirmwareRevision == right.targetFirmwareRevision;
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
    if (record.formatVersion >= 3U)
    {
        output << "SOURCE "
               << (record.sourceRestoreRequired ? 1U : 0U) << ' '
               << record.sourceQuad << ' '
               << record.sourceOriginalConfiguration;
        if (record.formatVersion >= 4U)
        {
            output << ' ' << record.sourceAppliedConfiguration;
        }
        output << '\n';
    }
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
    if (record.formatVersion == TunerRecoveryJournalV5FormatVersion)
        return SerializeTunerRecoveryJournalV5(record);

    TunerRecoverySerializationResult result;
    if (record.version5.has_value())
    {
        result.message =
            "A legacy recovery journal cannot contain v5 evidence.";
        return result;
    }
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
    {
        std::istringstream header(text);
        std::string headerMagic;
        std::uint32_t headerVersion = 0U;
        if ((header >> headerMagic >> headerVersion)
            && headerMagic == JournalMagic
            && headerVersion == TunerRecoveryJournalV5FormatVersion)
        {
            return ParseTunerRecoveryJournalV5(text);
        }
    }

    TunerRecoveryParseResult result;
    std::istringstream input(text);
    TunerRecoveryRecord record;
    std::string error;
    std::string magic;
    std::uint16_t phase = 0U;
    std::uint16_t previewRequired = 0U;
    std::uint16_t sourceRequired = 0U;
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

    if (record.formatVersion < 1U
        || record.formatVersion > TunerRecoveryJournalFormatVersion)
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
            error))
    {
        result.message = error.empty()
            ? "Malformed tuner recovery journal."
            : error;
        return result;
    }

    if (record.formatVersion >= 3U
        && (!ExpectToken(input, "SOURCE", error)
            || !ReadUnsigned(input, sourceRequired, "source state", error)
            || !ReadUnsigned(input, record.sourceQuad, "source quad", error)
            || !ReadUnsigned(
                input,
                record.sourceOriginalConfiguration,
                "source original configuration",
                error)
            || (record.formatVersion >= 4U
                && !ReadUnsigned(
                    input,
                    record.sourceAppliedConfiguration,
                    "source applied configuration",
                    error))))
    {
        result.message = error;
        return result;
    }

    if (!ExpectToken(input, "STATE", error)
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
    if (sourceRequired > 1U)
    {
        result.message = "Invalid source state in tuner recovery journal.";
        return result;
    }
    record.sourceRestoreRequired = sourceRequired == 1U;
    record.sourceAppliedConfigurationAvailable =
        record.formatVersion >= 4U && record.sourceRestoreRequired;
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
    const std::string& path,
    const TunerRecoveryJournalSaveRuntime& runtime)
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

    return InstallSerializedJournalAtomically(
        serialized.text,
        std::filesystem::path(path),
        AtomicJournalInstallMode::Upsert,
        runtime);
}

TunerRecoverySaveResult CreateTunerRecoveryJournalExclusive(
    const TunerRecoveryRecord& record,
    const std::string& path,
    const TunerRecoveryJournalSaveRuntime& runtime)
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
        result.message = "Recovery journal not created: "
            + serialized.message;
        return result;
    }
    return InstallSerializedJournalAtomically(
        serialized.text,
        std::filesystem::path(path),
        AtomicJournalInstallMode::Exclusive,
        runtime);
}

TunerRecoverySaveResult PromoteTunerRecoveryJournalV5Snapshot(
    const TunerRecoveryRecord& record,
    const std::string& path,
    const TunerRecoveryJournalSaveRuntime& runtime)
{
    TunerRecoverySaveResult result;
    if (path.empty())
    {
        result.message = "The tuner recovery-journal path is empty.";
        return result;
    }

    const auto loaded = LoadTunerRecoveryJournal(path);
    if (!loaded.success || !loaded.record.has_value()
        || loaded.record->formatVersion
            != TunerRecoveryJournalV5FormatVersion
        || !loaded.record->version5.has_value())
    {
        result.message =
            "The existing Prepared v5 recovery journal could not be trusted: "
            + loaded.message;
        return result;
    }
    const auto& prepared = *loaded.record->version5;
    if (prepared.sessionPhase != TuningSessionPhase::Preparing
        || !prepared.selectorParkingRequired
        || prepared.ownership.has_value()
        || prepared.liveRestoreSnapshot.has_value()
        || !prepared.deviations.empty())
    {
        result.message =
            "The existing journal is not the identity-only Prepared record.";
        return result;
    }

    if (record.formatVersion != TunerRecoveryJournalV5FormatVersion
        || !record.version5.has_value())
    {
        result.message = "The promoted recovery journal is not v5.";
        return result;
    }
    const auto& promoted = *record.version5;
    if (promoted.sessionPhase != prepared.sessionPhase
        || promoted.selectorParkingRequired
        || promoted.ownership.has_value()
        || !promoted.liveRestoreSnapshot.has_value()
        || !promoted.deviations.empty()
        || !SameEndpoint(promoted.endpoint, prepared.endpoint)
        || !SameIdentity(promoted.identity, prepared.identity))
    {
        result.message =
            "The promoted snapshot does not match its Prepared authority.";
        return result;
    }

    const auto serialized = SerializeTunerRecoveryJournal(record);
    if (!serialized.success)
    {
        result.message = "Recovery journal not promoted: "
            + serialized.message;
        return result;
    }
    return InstallSerializedJournalAtomically(
        serialized.text,
        std::filesystem::path(path),
        AtomicJournalInstallMode::ReplaceExisting,
        runtime);
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
