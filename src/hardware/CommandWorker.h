#ifndef FIDGET_HARDWARE_COMMAND_WORKER_H
#define FIDGET_HARDWARE_COMMAND_WORKER_H

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace fidget {

class CommandWorker
{
public:
    CommandWorker();
    ~CommandWorker();

    CommandWorker(const CommandWorker&) = delete;
    CommandWorker& operator=(const CommandWorker&) = delete;

    template<typename Function>
    [[nodiscard]] auto Post(Function&& function)
        -> std::future<
            std::invoke_result_t<std::decay_t<Function>&>>
    {
        using Callable = std::decay_t<Function>;
        using Result = std::invoke_result_t<Callable&>;

        auto promise = std::make_shared<std::promise<Result>>();
        auto future = promise->get_future();
        auto callable = std::make_shared<Callable>(
            std::forward<Function>(function));

        const auto task = [promise, callable]() mutable {
            try
            {
                if constexpr (std::is_void_v<Result>)
                {
                    std::invoke(*callable);
                    promise->set_value();
                }
                else
                {
                    promise->set_value(std::invoke(*callable));
                }
            }
            catch (...)
            {
                promise->set_exception(std::current_exception());
            }
        };

        if (!Enqueue(task))
        {
            promise->set_exception(std::make_exception_ptr(
                std::runtime_error("Command worker is stopped")));
        }
        return future;
    }

    void StopAndJoin();

    [[nodiscard]] bool IsAcceptingTasks() const;

private:
    [[nodiscard]] bool Enqueue(std::function<void()> task);
    void Run();

    mutable std::mutex mutex_;
    std::condition_variable taskAvailable_;
    std::deque<std::function<void()>> tasks_;
    bool acceptingTasks_ = true;
    bool stopRequested_ = false;
    std::thread worker_;
};

} // namespace fidget

#endif
