#ifndef FIDGET_TESTS_FAKE_COMMAND_TRANSPORT_H
#define FIDGET_TESTS_FAKE_COMMAND_TRANSPORT_H

#include "hardware/Transport.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace fidget::test {

struct FakeReceiveAction
{
    TransportReceiveStatus status = TransportReceiveStatus::Timeout;
    std::vector<std::byte> datagram;
    std::string error = "receive: MVLC response timed out";

    [[nodiscard]] static FakeReceiveAction Datagram(
        std::vector<std::byte> bytes)
    {
        FakeReceiveAction action;
        action.status = TransportReceiveStatus::Received;
        action.datagram = std::move(bytes);
        action.error.clear();
        return action;
    }

    [[nodiscard]] static FakeReceiveAction Timeout()
    {
        return {};
    }

    [[nodiscard]] static FakeReceiveAction Error(std::string message)
    {
        FakeReceiveAction action;
        action.status = TransportReceiveStatus::Error;
        action.error = std::move(message);
        return action;
    }
};

struct FakeCommandExchange
{
    std::vector<std::byte> expectedRequest;
    std::vector<FakeReceiveAction> receiveActions;
};

class FakeCommandTransport final : public ICommandTransport
{
public:
    void QueueExchange(FakeCommandExchange exchange)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        exchanges_.push_back(std::move(exchange));
    }

    void SetOpenError(std::string error)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        openError_ = std::move(error);
    }

    void SetReceiveHook(std::function<void(std::size_t)> hook)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        receiveHook_ = std::move(hook);
    }

    void SetSendHook(
        std::function<void(const std::vector<std::byte>&)> hook)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        sendHook_ = std::move(hook);
    }

    [[nodiscard]] std::vector<std::vector<std::byte>> SentRequests() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return sentRequests_;
    }

    [[nodiscard]] std::vector<std::size_t> ReceiveCapacities() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return receiveCapacities_;
    }

    [[nodiscard]] std::string OpenedHost() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return openedHost_;
    }

    [[nodiscard]] std::uint16_t OpenedPort() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return openedPort_;
    }

    [[nodiscard]] bool IsOpen() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return open_;
    }

    [[nodiscard]] TransportOperationResult Open(
        const std::string& host,
        std::uint16_t port) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        openedHost_ = host;
        openedPort_ = port;
        if (!openError_.empty())
        {
            return {false, openError_};
        }

        open_ = true;
        return {true, {}};
    }

    [[nodiscard]] TransportOperationResult Send(
        const std::byte* data,
        std::size_t size) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!open_)
        {
            return {false, "fake send: transport is closed"};
        }

        std::vector<std::byte> request(data, data + size);
        sentRequests_.push_back(request);
        if (exchanges_.empty())
        {
            return {false, "fake send: unexpected request"};
        }

        auto exchange = std::move(exchanges_.front());
        exchanges_.pop_front();
        if (request != exchange.expectedRequest)
        {
            return {false, "fake send: request mismatch"};
        }

        if (sendHook_)
        {
            sendHook_(request);
        }

        for (auto& action : exchange.receiveActions)
        {
            pendingReceives_.push_back(std::move(action));
        }
        return {true, {}};
    }

    [[nodiscard]] TransportReceiveResult Receive(
        std::byte* buffer,
        std::size_t capacity,
        int timeoutMilliseconds) override
    {
        std::function<void(std::size_t)> hook;
        std::size_t receiveCount = 0U;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            receiveCapacities_.push_back(capacity);
            receiveTimeouts_.push_back(timeoutMilliseconds);
            hook = receiveHook_;
            receiveCount = receiveCapacities_.size();
        }
        if (hook)
        {
            hook(receiveCount);
        }

        const std::lock_guard<std::mutex> lock(mutex_);

        if (!open_)
        {
            return {
                TransportReceiveStatus::Error,
                0U,
                "fake receive: transport is closed",
            };
        }

        if (pendingReceives_.empty())
        {
            return {
                TransportReceiveStatus::Timeout,
                0U,
                "receive: MVLC response timed out",
            };
        }

        auto action = std::move(pendingReceives_.front());
        pendingReceives_.pop_front();
        if (action.status != TransportReceiveStatus::Received)
        {
            return {action.status, 0U, std::move(action.error)};
        }
        if (action.datagram.size() > capacity)
        {
            return {
                TransportReceiveStatus::Error,
                0U,
                "fake receive: datagram exceeds buffer",
            };
        }

        std::copy(action.datagram.begin(), action.datagram.end(), buffer);
        return {
            TransportReceiveStatus::Received,
            action.datagram.size(),
            {},
        };
    }

    void Close() noexcept override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        open_ = false;
    }

private:
    mutable std::mutex mutex_;
    bool open_ = false;
    std::string openError_;
    std::string openedHost_;
    std::uint16_t openedPort_ = 0U;
    std::deque<FakeCommandExchange> exchanges_;
    std::deque<FakeReceiveAction> pendingReceives_;
    std::vector<std::vector<std::byte>> sentRequests_;
    std::vector<std::size_t> receiveCapacities_;
    std::vector<int> receiveTimeouts_;
    std::function<void(std::size_t)> receiveHook_;
    std::function<void(const std::vector<std::byte>&)> sendHook_;
};

} // namespace fidget::test

#endif
