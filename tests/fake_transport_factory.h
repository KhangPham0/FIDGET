#ifndef FIDGET_TESTS_FAKE_TRANSPORT_FACTORY_H
#define FIDGET_TESTS_FAKE_TRANSPORT_FACTORY_H

#include "fake_command_transport.h"
#include "hardware/TransportFactory.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace fidget::test {

class SharedFakeCommandTransport final : public ICommandTransport
{
public:
    explicit SharedFakeCommandTransport(
        std::shared_ptr<FakeCommandTransport> transport)
        : transport_(std::move(transport))
    {
    }

    [[nodiscard]] TransportOperationResult Open(
        const std::string& host,
        std::uint16_t port) override
    {
        return transport_->Open(host, port);
    }

    [[nodiscard]] TransportOperationResult Send(
        const std::byte* data,
        std::size_t size) override
    {
        return transport_->Send(data, size);
    }

    [[nodiscard]] TransportReceiveResult Receive(
        std::byte* buffer,
        std::size_t capacity,
        int timeoutMilliseconds) override
    {
        return transport_->Receive(
            buffer, capacity, timeoutMilliseconds);
    }

    void Close() noexcept override
    {
        transport_->Close();
    }

private:
    std::shared_ptr<FakeCommandTransport> transport_;
};

class FakeDataReceiver final : public IDataReceiver
{
public:
    [[nodiscard]] TransportOperationResult Open(
        const std::string&,
        std::uint16_t) override
    {
        open_ = true;
        return {true, {}};
    }

    [[nodiscard]] TransportOperationResult Send(
        const std::byte*,
        std::size_t) override
    {
        return open_
            ? TransportOperationResult{true, {}}
            : TransportOperationResult{false, "fake data receiver is closed"};
    }

    [[nodiscard]] TransportReceiveResult Receive(
        std::byte*,
        std::size_t,
        int) override
    {
        return open_
            ? TransportReceiveResult{
                  TransportReceiveStatus::Timeout,
                  0U,
                  "receive: MVLC data timed out",
              }
            : TransportReceiveResult{
                  TransportReceiveStatus::Error,
                  0U,
                  "fake data receiver is closed",
              };
    }

    void Close() noexcept override
    {
        open_ = false;
    }

private:
    bool open_ = false;
};

class FakeTransportFactory final : public ITransportFactory
{
public:
    explicit FakeTransportFactory(
        std::unique_ptr<FakeCommandTransport> commandTransport)
        : commandTransport_(std::move(commandTransport))
    {
    }

    [[nodiscard]] TransportFactoryResult Create(
        const TransportEndpointRequest& request) override
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        requests_.push_back(request);
        return {
            std::make_unique<TransportSession>(
                std::make_unique<SharedFakeCommandTransport>(
                    commandTransport_),
                std::make_unique<FakeDataReceiver>()),
            {},
        };
    }

    [[nodiscard]] TransportFactoryResult Create(
        const CrateProject& project) override
    {
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            projects_.push_back(project);
        }
        return ITransportFactory::Create(project);
    }

    [[nodiscard]] std::size_t CreateCount() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return requests_.size();
    }

    [[nodiscard]] std::vector<TransportEndpointRequest> Requests() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return requests_;
    }

    [[nodiscard]] std::vector<CrateProject> Projects() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return projects_;
    }

private:
    std::shared_ptr<FakeCommandTransport> commandTransport_;
    mutable std::mutex mutex_;
    std::vector<TransportEndpointRequest> requests_;
    std::vector<CrateProject> projects_;
};

inline std::unique_ptr<ITransportFactory> MakeFakeTransportFactory(
    std::unique_ptr<FakeCommandTransport> commandTransport)
{
    return std::make_unique<FakeTransportFactory>(
        std::move(commandTransport));
}

} // namespace fidget::test

#endif
