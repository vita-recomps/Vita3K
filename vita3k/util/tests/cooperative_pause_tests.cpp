// SPDX-License-Identifier: GPL-2.0-or-later
#include <util/cooperative_pause.h>
#include <threads/queue.h>

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

static void require(bool condition, const char *message) {
    if (!condition)
        throw std::runtime_error(message);
}

int main() {
    CooperativePause gate;
    std::atomic<bool> canceled{ false };
    const auto cancel = [&] { return canceled.load(); };
    require(gate.wait(cancel), "unpaused execution failed");
    gate.pause();
    std::promise<void> entered;
    auto worker = std::async(std::launch::async, [&] {
        entered.set_value();
        return gate.wait(cancel);
    });
    entered.get_future().wait();
    require(worker.wait_for(30ms) == std::future_status::timeout, "paused worker advanced");
    gate.resume();
    require(worker.get(), "resume failed");

    // A guest becoming runnable after pause must observe the persistent state too.
    gate.pause();
    auto late = std::async(std::launch::async, [&] { return gate.wait(cancel); });
    require(late.wait_for(30ms) == std::future_status::timeout, "late wake bypassed pause");
    canceled = true;
    gate.notify();
    require(!late.get(), "cancellation did not release a waiter");
    require(gate.paused(), "canceling one guest resumed the session");
    canceled = false;
    gate.resume();

    for (int i = 0; i < 1000; ++i) {
        gate.pause();
        auto rapid = std::async(std::launch::async, [&] { return gate.wait(cancel); });
        gate.resume();
        require(rapid.get(), "lost rapid resume notification");
    }

    // Cancellation is published before notify, including a race with entering wait.
    for (int i = 0; i < 1000; ++i) {
        canceled = false;
        gate.pause();
        auto exiting = std::async(std::launch::async, [&] { return gate.wait(cancel); });
        canceled = true;
        gate.notify();
        require(exiting.wait_for(2s) == std::future_status::ready, "lost cancellation notification");
        require(!exiting.get(), "canceled guest was admitted");
        gate.resume();
    }
    canceled = false;

    // Model a stop returning to a nested execution loop, rather than run_loop.
    // This tests the gate/re-entry contract; it does not instantiate Dynarmic.
    gate.pause();
    std::atomic<int> entries{ 0 };
    auto nested = std::async(std::launch::async, [&] {
        if (gate.wait(cancel))
            ++entries;
    });
    require(nested.wait_for(30ms) == std::future_status::timeout, "nested re-entry bypassed pause");
    require(entries == 0, "nested execution advanced while paused");
    gate.resume();
    nested.get();
    require(entries == 1, "nested execution failed to resume");

    gate.pause();
    std::vector<std::future<bool>> parked;
    for (int i = 0; i < 8; ++i)
        parked.push_back(std::async(std::launch::async, [&] { return gate.wait(cancel); }));
    gate.resume();
    for (auto &thread : parked)
        require(thread.get(), "shutdown failed to release parked workers");

    // process_exit can encounter a display queue with both push() and wait_space() blocked.
    for (int i = 0; i < 500; ++i) {
        Queue<int> full;
        full.maxPendingCount_ = 1;
        require(full.push(1), "could not fill the queue");
        auto producer = std::async(std::launch::async, [&] { return full.push(2); });
        auto waiter = std::async(std::launch::async, [&] { return full.wait_space(); });
        Queue<int> empty;
        auto consumer = std::async(std::launch::async, [&] { return empty.pop(); });
        full.abort();
        empty.abort();
        require(!producer.get() && !waiter.get() && !consumer.get(), "an aborted queue admitted a waiter");
    }

    std::cout << "PASS: fast path, pause/resume, late wake, 1000 resume races, 1000 cancellation races, nested re-entry contract, 8-worker shutdown\n";
}
