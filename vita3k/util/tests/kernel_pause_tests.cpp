// SPDX-License-Identifier: GPL-2.0-or-later
#include <cpu/state.h>
#include <kernel/state.h>
#include <kernel/thread/thread_state.h>
#include <mem/functions.h>
#include <mem/state.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <future>
#include <iostream>

using namespace std::chrono_literals;

static void require(bool condition, const char *message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(1);
    }
}

int main() {
    MemState mem;
    KernelState kernel;
    require(init(mem, false), "guest memory initialization failed");

    std::atomic<unsigned> imports{ 0 };
    std::promise<void> inside_import, release_import;
    auto release = release_import.get_future();
    const auto call_import = [&](CPUState &, uint32_t nid, SceUID) {
        require(nid == 42, "unexpected import NID");
        if (++imports == 2) {
            inside_import.set_value();
            release.wait();
        }
    };
    require(kernel.init(mem, call_import, false), "kernel initialization failed");

    auto code = alloc_block(mem, 4, "test guest code");
    auto *insns = code.get_ptr<uint16_t>().get(mem);
    insns[0] = 0x202a; // movs r0, #42
    insns[1] = 0x4770; // bx lr
    const Ptr<const void> entry(code.get() | 1u);

    auto guest = kernel.create_thread(mem, "test guest", entry);
    require(guest != nullptr, "could not create guest thread");
    kernel.pause_threads();
    require(guest->start(0, {}) == 0, "could not start guest thread");
    {
        std::unique_lock lock(guest->mutex);
        require(!guest->status_cond.wait_for(lock, 30ms, [&] { return guest->status == ThreadStatus::dormant; }),
            "run_loop executed guest code during pause");
    }
    kernel.resume_threads();
    {
        std::unique_lock lock(guest->mutex);
        require(guest->status_cond.wait_for(lock, 5s, [&] { return guest->status == ThreadStatus::dormant; }),
            "run_loop did not resume");
        require(guest->returned_value == 42, "guest code did not run to its return");
    }

    kernel.pause_threads();
    auto before = std::async(std::launch::async, [&] { guest->cpu->import_handler(*guest->cpu, 42); });
    require(before.wait_for(30ms) == std::future_status::timeout && imports == 0, "an import entered during pause");
    kernel.resume_threads();
    before.get();
    require(imports == 1, "import did not resume");

    auto after = std::async(std::launch::async, [&] { guest->cpu->import_handler(*guest->cpu, 42); });
    inside_import.get_future().wait();
    kernel.pause_threads();
    release_import.set_value();
    require(after.wait_for(30ms) == std::future_status::timeout, "an import returned to guest code during pause");
    kernel.resume_threads();
    after.get();
    guest.reset();

    kernel.pause_threads();
    auto parked = kernel.create_thread(mem, "guest to stop", entry);
    require(parked && parked->start(0, {}) == 0, "could not start guest for shutdown");
    parked.reset();
    kernel.process_exit();
    require(!kernel.is_threads_paused() && kernel.threads.empty(), "process_exit did not release and join all guests");
    code = nullptr;
    kernel.deinit(mem);
    deinit_mem(mem);
}
