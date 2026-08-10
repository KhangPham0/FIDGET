#include "hardware/CommandWorker.h"

#include <utility>

namespace fidget {

CommandWorker::CommandWorker()
    : worker_(&CommandWorker::Run, this)
{
}

CommandWorker::~CommandWorker()
{
    StopAndJoin();
}

bool CommandWorker::Enqueue(std::function<void()> task)
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!acceptingTasks_)
        {
            return false;
        }
        tasks_.push_back(std::move(task));
    }
    taskAvailable_.notify_one();
    return true;
}

void CommandWorker::StopAndJoin()
{
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        acceptingTasks_ = false;
        stopRequested_ = true;
    }
    taskAvailable_.notify_one();

    if (worker_.joinable())
    {
        worker_.join();
    }
}

bool CommandWorker::IsAcceptingTasks() const
{
    const std::lock_guard<std::mutex> lock(mutex_);
    return acceptingTasks_;
}

void CommandWorker::Run()
{
    while (true)
    {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            taskAvailable_.wait(lock, [this] {
                return stopRequested_ || !tasks_.empty();
            });

            if (tasks_.empty())
            {
                if (stopRequested_)
                {
                    return;
                }
                continue;
            }

            task = std::move(tasks_.front());
            tasks_.pop_front();
        }

        task();
    }
}

} // namespace fidget
