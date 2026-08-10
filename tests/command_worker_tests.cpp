#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest/doctest.h"

#include "hardware/CommandWorker.h"

#include <atomic>
#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

TEST_CASE("the command worker executes posted tasks serially in order")
{
    using namespace fidget;

    CommandWorker worker;
    std::vector<int> executionOrder;
    std::atomic<int> activeTasks{0};
    std::atomic<int> maximumActiveTasks{0};

    const auto makeTask = [&](const int value) {
        return [&, value] {
            const int active = activeTasks.fetch_add(1) + 1;
            int maximum = maximumActiveTasks.load();
            while (maximum < active
                   && !maximumActiveTasks.compare_exchange_weak(
                       maximum, active))
            {
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            executionOrder.push_back(value);
            activeTasks.fetch_sub(1);
            return value * 10;
        };
    };

    auto first = worker.Post(makeTask(1));
    auto second = worker.Post(makeTask(2));
    auto third = worker.Post(makeTask(3));

    CHECK(first.get() == 10);
    CHECK(second.get() == 20);
    CHECK(third.get() == 30);
    CHECK(executionOrder == std::vector<int>{1, 2, 3});
    CHECK(maximumActiveTasks.load() == 1);

    worker.StopAndJoin();
}

TEST_CASE("command worker shutdown drains queued tasks and rejects new work")
{
    using namespace fidget;

    CommandWorker worker;
    std::atomic<int> completedTasks{0};
    auto first = worker.Post([&completedTasks] {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        ++completedTasks;
    });
    auto second = worker.Post([&completedTasks] {
        ++completedTasks;
        return 42;
    });

    worker.StopAndJoin();
    first.get();
    CHECK(second.get() == 42);
    CHECK(completedTasks.load() == 2);
    CHECK_FALSE(worker.IsAcceptingTasks());

    auto rejected = worker.Post([] { return 7; });
    CHECK_THROWS_WITH_AS(
        rejected.get(),
        "Command worker is stopped",
        std::runtime_error);

    worker.StopAndJoin();
}

TEST_CASE("task failures do not stop the command worker")
{
    using namespace fidget;

    CommandWorker worker;
    auto failed = worker.Post([]() -> int {
        throw std::runtime_error("task failed");
    });
    auto following = worker.Post([] { return 2051; });

    CHECK_THROWS_WITH_AS(failed.get(), "task failed", std::runtime_error);
    CHECK(following.get() == 2051);
    worker.StopAndJoin();
}
