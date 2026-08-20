#ifndef FIDGET_HARDWARE_SSH_BRIDGE_PROCESS_H
#define FIDGET_HARDWARE_SSH_BRIDGE_PROCESS_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

namespace fidget {

class BridgeConnection;
class SshBridgeProcess;

struct SshBridgeProcessStartResult
{
    bool success = false;
    std::shared_ptr<SshBridgeProcess> process;
    std::string error;
};

class SshBridgeProcess
{
public:
    ~SshBridgeProcess();

    SshBridgeProcess(const SshBridgeProcess&) = delete;
    SshBridgeProcess& operator=(const SshBridgeProcess&) = delete;

    // SSH authentication must be non-interactive, normally through a key and
    // an agent. FIDGET has no terminal in which to answer an SSH prompt.
    [[nodiscard]] static SshBridgeProcessStartResult StartSsh(
        const std::string& destination,
        const std::string& remoteCommand,
        const std::string& mvlcHost,
        std::uint16_t commandPort);

    // This direct form supports tests and the direct-child integration rung.
    // arguments[0] is the executable and no shell is involved.
    [[nodiscard]] static SshBridgeProcessStartResult StartProgram(
        const std::vector<std::string>& arguments);

    [[nodiscard]] std::shared_ptr<BridgeConnection> Connection() const;
    [[nodiscard]] std::string CapturedStderr() const;
    void Stop() noexcept;

private:
    SshBridgeProcess(
        pid_t childProcessId,
        int childStdoutDescriptor,
        int childStdinDescriptor,
        int childStderrDescriptor);

    void StartStderrReader();
    void ReadStderr();
    void AppendStderr(const char* data, std::size_t size);
    [[nodiscard]] bool WaitForChild(int timeoutMilliseconds) noexcept;
    void ReapChild() noexcept;
    void CloseStderrDescriptor() noexcept;

    pid_t childProcessId_ = -1;
    std::shared_ptr<BridgeConnection> connection_;
    int stderrDescriptor_ = -1;
    std::atomic<bool> stopStderr_{false};
    mutable std::mutex stderrMutex_;
    std::string stderrText_;
    std::mutex stopMutex_;
    bool stopped_ = false;

    // Keep the thread last so every object it uses is initialized first.
    std::thread stderrThread_;
};

} // namespace fidget

#endif
