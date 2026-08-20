#include "hardware/SshBridgeProcess.h"

#include "hardware/BridgeConnection.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <exception>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

extern char** environ;

namespace fidget {
namespace {

constexpr int StderrPollIntervalMilliseconds = 100;
constexpr int GracefulExitTimeoutMilliseconds = 500;
constexpr int TerminationTimeoutMilliseconds = 250;
constexpr int ChildWaitSliceMilliseconds = 10;
constexpr std::size_t MaximumCapturedStderrSize = 64U * 1024U;

struct PipeDescriptors
{
    int readDescriptor = -1;
    int writeDescriptor = -1;
};

std::string ErrnoMessage(const char* operation)
{
    return std::string(operation) + ": " + std::strerror(errno);
}

void CloseDescriptor(int& descriptor)
{
    if (descriptor >= 0)
    {
        ::close(descriptor);
        descriptor = -1;
    }
}

bool PreparePipeDescriptor(int& descriptor, std::string& error)
{
    if (descriptor <= STDERR_FILENO)
    {
        const int movedDescriptor = ::fcntl(descriptor, F_DUPFD, 3);
        if (movedDescriptor < 0)
        {
            error = ErrnoMessage("fcntl duplicate");
            return false;
        }
        ::close(descriptor);
        descriptor = movedDescriptor;
    }

    const int flags = ::fcntl(descriptor, F_GETFD);
    if (flags < 0
        || ::fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) != 0)
    {
        error = ErrnoMessage("fcntl close-on-exec");
        return false;
    }
    return true;
}

std::string ErrorCodeMessage(const char* operation, const int errorCode)
{
    return std::string(operation) + ": " + std::strerror(errorCode);
}

bool OpenPipe(PipeDescriptors& descriptors, std::string& error)
{
    std::array<int, 2> pipeDescriptors{};
    if (::pipe(pipeDescriptors.data()) != 0)
    {
        error = ErrnoMessage("pipe");
        return false;
    }

    descriptors.readDescriptor = pipeDescriptors[0];
    descriptors.writeDescriptor = pipeDescriptors[1];
    if (!PreparePipeDescriptor(descriptors.readDescriptor, error)
        || !PreparePipeDescriptor(descriptors.writeDescriptor, error))
    {
        CloseDescriptor(descriptors.readDescriptor);
        CloseDescriptor(descriptors.writeDescriptor);
        return false;
    }
    return true;
}

void ClosePipe(PipeDescriptors& descriptors)
{
    CloseDescriptor(descriptors.readDescriptor);
    CloseDescriptor(descriptors.writeDescriptor);
}

bool AddDuplicateAction(
    posix_spawn_file_actions_t& actions,
    const int source,
    const int destination,
    std::string& error)
{
    const int result = ::posix_spawn_file_actions_adddup2(
        &actions, source, destination);
    if (result != 0)
    {
        error = ErrorCodeMessage("posix_spawn adddup2", result);
        return false;
    }
    return true;
}

bool AddCloseAction(
    posix_spawn_file_actions_t& actions,
    const int descriptor,
    std::string& error)
{
    const int result = ::posix_spawn_file_actions_addclose(
        &actions, descriptor);
    if (result != 0)
    {
        error = ErrorCodeMessage("posix_spawn addclose", result);
        return false;
    }
    return true;
}

bool ConfigureChildDescriptors(
    posix_spawn_file_actions_t& actions,
    const PipeDescriptors& childInput,
    const PipeDescriptors& childOutput,
    const PipeDescriptors& childError,
    std::string& error)
{
    if (!AddDuplicateAction(
            actions, childInput.readDescriptor, STDIN_FILENO, error)
        || !AddDuplicateAction(
            actions, childOutput.writeDescriptor, STDOUT_FILENO, error)
        || !AddDuplicateAction(
            actions, childError.writeDescriptor, STDERR_FILENO, error))
    {
        return false;
    }

    const std::array<int, 6> descriptors{
        childInput.readDescriptor,
        childInput.writeDescriptor,
        childOutput.readDescriptor,
        childOutput.writeDescriptor,
        childError.readDescriptor,
        childError.writeDescriptor,
    };
    for (const int descriptor : descriptors)
    {
        if (!AddCloseAction(actions, descriptor, error))
        {
            return false;
        }
    }
    return true;
}

std::vector<char*> MakeArgumentPointers(
    std::vector<std::string>& arguments)
{
    std::vector<char*> pointers;
    pointers.reserve(arguments.size() + 1U);
    for (auto& argument : arguments)
    {
        pointers.push_back(argument.data());
    }
    pointers.push_back(nullptr);
    return pointers;
}

} // namespace

SshBridgeProcess::SshBridgeProcess(
    const pid_t childProcessId,
    const int childStdoutDescriptor,
    const int childStdinDescriptor,
    const int childStderrDescriptor)
    : childProcessId_(childProcessId)
    , connection_(std::make_shared<BridgeConnection>(
          childStdoutDescriptor, childStdinDescriptor))
    , stderrDescriptor_(childStderrDescriptor)
{
}

SshBridgeProcess::~SshBridgeProcess()
{
    Stop();
}

std::vector<std::string> BuildSshBridgeProcessArguments(
    const std::string& destination,
    const std::string& remoteCommand,
    const std::string& mvlcHost,
    const std::uint16_t commandPort)
{
    return {
        "ssh",
        "-o",
        "BatchMode=yes",
        destination,
        remoteCommand,
        mvlcHost,
        std::to_string(commandPort),
    };
}

SshBridgeProcessStartResult SshBridgeProcess::StartSsh(
    const std::string& destination,
    const std::string& remoteCommand,
    const std::string& mvlcHost,
    const std::uint16_t commandPort)
{
    return StartProgram(BuildSshBridgeProcessArguments(
        destination, remoteCommand, mvlcHost, commandPort));
}

SshBridgeProcessStartResult SshBridgeProcess::StartProgram(
    const std::vector<std::string>& arguments)
{
    if (arguments.empty() || arguments.front().empty())
    {
        return {false, {}, "Bridge process executable is empty"};
    }

    PipeDescriptors childInput;
    PipeDescriptors childOutput;
    PipeDescriptors childError;
    std::string error;
    if (!OpenPipe(childInput, error)
        || !OpenPipe(childOutput, error)
        || !OpenPipe(childError, error))
    {
        ClosePipe(childInput);
        ClosePipe(childOutput);
        ClosePipe(childError);
        return {false, {}, std::move(error)};
    }

    posix_spawn_file_actions_t actions;
    const int initializeResult = ::posix_spawn_file_actions_init(&actions);
    if (initializeResult != 0)
    {
        ClosePipe(childInput);
        ClosePipe(childOutput);
        ClosePipe(childError);
        return {
            false,
            {},
            ErrorCodeMessage(
                "posix_spawn file-actions init", initializeResult),
        };
    }

    if (!ConfigureChildDescriptors(
            actions, childInput, childOutput, childError, error))
    {
        ::posix_spawn_file_actions_destroy(&actions);
        ClosePipe(childInput);
        ClosePipe(childOutput);
        ClosePipe(childError);
        return {false, {}, std::move(error)};
    }

    std::vector<std::string> mutableArguments = arguments;
    std::vector<char*> argumentPointers = MakeArgumentPointers(
        mutableArguments);
    pid_t childProcessId = -1;
    const int spawnResult = ::posix_spawnp(
        &childProcessId,
        argumentPointers.front(),
        &actions,
        nullptr,
        argumentPointers.data(),
        environ);
    ::posix_spawn_file_actions_destroy(&actions);

    CloseDescriptor(childInput.readDescriptor);
    CloseDescriptor(childOutput.writeDescriptor);
    CloseDescriptor(childError.writeDescriptor);

    if (spawnResult != 0)
    {
        CloseDescriptor(childInput.writeDescriptor);
        CloseDescriptor(childOutput.readDescriptor);
        CloseDescriptor(childError.readDescriptor);
        return {
            false,
            {},
            ErrorCodeMessage("posix_spawn", spawnResult),
        };
    }

    std::shared_ptr<SshBridgeProcess> process;
    try
    {
        process.reset(new SshBridgeProcess(
            childProcessId,
            childOutput.readDescriptor,
            childInput.writeDescriptor,
            childError.readDescriptor));
        process->StartStderrReader();
    }
    catch (const std::exception& exception)
    {
        const std::string startError =
            std::string("Could not initialize bridge process: ")
            + exception.what();
        if (process)
        {
            process.reset();
        }
        else
        {
            CloseDescriptor(childInput.writeDescriptor);
            CloseDescriptor(childOutput.readDescriptor);
            CloseDescriptor(childError.readDescriptor);
            ::kill(childProcessId, SIGKILL);
            int status = 0;
            while (::waitpid(childProcessId, &status, 0) < 0
                   && errno == EINTR)
            {
            }
        }
        return {false, {}, startError};
    }
    return {true, std::move(process), {}};
}

std::shared_ptr<BridgeConnection> SshBridgeProcess::Connection() const
{
    return connection_;
}

std::string SshBridgeProcess::CapturedStderr() const
{
    std::lock_guard<std::mutex> lock(stderrMutex_);
    return stderrText_;
}

void SshBridgeProcess::StartStderrReader()
{
    stderrThread_ = std::thread(&SshBridgeProcess::ReadStderr, this);
}

void SshBridgeProcess::ReadStderr()
{
    std::array<char, 4096U> buffer{};
    while (true)
    {
        pollfd descriptor{};
        descriptor.fd = stderrDescriptor_;
        descriptor.events = POLLIN;

        int pollResult = -1;
        do
        {
            pollResult = ::poll(
                &descriptor, 1U, StderrPollIntervalMilliseconds);
        } while (pollResult < 0 && errno == EINTR);

        if (pollResult < 0)
        {
            const std::string pollError = ErrnoMessage(
                "bridge stderr poll");
            AppendStderr(pollError.data(), pollError.size());
            return;
        }
        if (pollResult == 0)
        {
            if (stopStderr_.load())
            {
                return;
            }
            continue;
        }
        if ((descriptor.revents & POLLNVAL) != 0)
        {
            return;
        }
        if ((descriptor.revents & (POLLIN | POLLHUP | POLLERR)) == 0)
        {
            continue;
        }

        ssize_t received = -1;
        do
        {
            received = ::read(
                stderrDescriptor_, buffer.data(), buffer.size());
        } while (received < 0 && errno == EINTR);
        if (received > 0)
        {
            AppendStderr(buffer.data(), static_cast<std::size_t>(received));
            continue;
        }
        if (received < 0)
        {
            const std::string readError = ErrnoMessage("bridge stderr read");
            AppendStderr(readError.data(), readError.size());
        }
        return;
    }
}

void SshBridgeProcess::AppendStderr(
    const char* data, const std::size_t size)
{
    if (size == 0U)
    {
        return;
    }

    std::lock_guard<std::mutex> lock(stderrMutex_);
    if (size >= MaximumCapturedStderrSize)
    {
        stderrText_.assign(
            data + size - MaximumCapturedStderrSize,
            MaximumCapturedStderrSize);
        return;
    }

    const std::size_t excess = stderrText_.size() + size
        > MaximumCapturedStderrSize
        ? stderrText_.size() + size - MaximumCapturedStderrSize
        : 0U;
    if (excess > 0U)
    {
        stderrText_.erase(0U, excess);
    }
    stderrText_.append(data, size);
}

void SshBridgeProcess::Stop() noexcept
{
    std::lock_guard<std::mutex> lock(stopMutex_);
    if (stopped_)
    {
        return;
    }
    stopped_ = true;

    if (connection_)
    {
        connection_->Close();
    }

    if (!WaitForChild(GracefulExitTimeoutMilliseconds)
        && childProcessId_ > 0)
    {
        ::kill(childProcessId_, SIGTERM);
        if (!WaitForChild(TerminationTimeoutMilliseconds)
            && childProcessId_ > 0)
        {
            ::kill(childProcessId_, SIGKILL);
            ReapChild();
        }
    }

    stopStderr_.store(true);
    if (stderrThread_.joinable())
    {
        stderrThread_.join();
    }
    CloseStderrDescriptor();
}

bool SshBridgeProcess::WaitForChild(
    const int timeoutMilliseconds) noexcept
{
    if (childProcessId_ <= 0)
    {
        return true;
    }

    int waitedMilliseconds = 0;
    while (waitedMilliseconds <= timeoutMilliseconds)
    {
        int status = 0;
        const pid_t result = ::waitpid(childProcessId_, &status, WNOHANG);
        if (result == childProcessId_)
        {
            childProcessId_ = -1;
            return true;
        }
        if (result < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            if (errno == ECHILD)
            {
                childProcessId_ = -1;
                return true;
            }
            return false;
        }
        if (waitedMilliseconds == timeoutMilliseconds)
        {
            break;
        }

        const int remaining = timeoutMilliseconds - waitedMilliseconds;
        const int waitSlice = std::min(
            ChildWaitSliceMilliseconds, remaining);
        std::this_thread::sleep_for(std::chrono::milliseconds(waitSlice));
        waitedMilliseconds += waitSlice;
    }
    return false;
}

void SshBridgeProcess::ReapChild() noexcept
{
    if (childProcessId_ <= 0)
    {
        return;
    }

    int status = 0;
    pid_t result = -1;
    do
    {
        result = ::waitpid(childProcessId_, &status, 0);
    } while (result < 0 && errno == EINTR);
    childProcessId_ = -1;
}

void SshBridgeProcess::CloseStderrDescriptor() noexcept
{
    CloseDescriptor(stderrDescriptor_);
}

} // namespace fidget
