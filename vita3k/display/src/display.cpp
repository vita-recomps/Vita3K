// Vita3K emulator project
// Copyright (C) 2026 Vita3K team
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.

#include <display/functions.h>

#include <dialog/state.h>
#include <display/state.h>
#include <emuenv/state.h>
#include <kernel/state.h>
#include <renderer/state.h>

#include <chrono>
#include <motion/functions.h>
#include <thread>
#include <touch/functions.h>
#include <util/thread_qos.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

// Code heavily influenced by PPSSSPP's SceDisplay.cpp

static constexpr int DEFAULT_VBLANK_RATE = 60;
// how many cycles do we need to see before we start predicting the next frame
static constexpr int predict_threshold = 3;
static constexpr int max_expected_swapchain_size = 6;

namespace {

class VblankPacer {
public:
#ifdef _WIN32
    VblankPacer() {
        winmm = LoadLibraryW(L"winmm.dll");
        if (winmm) {
            time_begin_period = reinterpret_cast<TimerPeriodFn>(GetProcAddress(winmm, "timeBeginPeriod"));
            time_end_period = reinterpret_cast<TimerPeriodFn>(GetProcAddress(winmm, "timeEndPeriod"));
            if (time_begin_period && time_end_period && time_begin_period(1) == 0)
                period_active = true;
        }
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer)
            timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    }

    ~VblankPacer() {
        if (timer)
            CloseHandle(timer);
        if (period_active && time_end_period)
            time_end_period(1);
        if (winmm)
            FreeLibrary(winmm);
    }

    VblankPacer(const VblankPacer &) = delete;
    VblankPacer &operator=(const VblankPacer &) = delete;

    void sleep_until(std::chrono::steady_clock::time_point target) const {
        constexpr auto spin_margin = std::chrono::microseconds(400);
        constexpr auto min_sleep = std::chrono::microseconds(500);
        const auto now = std::chrono::steady_clock::now();
        if (target <= now)
            return;
        const auto remaining = target - now;
        if (timer && remaining > min_sleep) {
            LARGE_INTEGER due_time{};
            const auto wait_us = std::chrono::duration_cast<std::chrono::microseconds>(remaining - spin_margin).count();
            due_time.QuadPart = -wait_us * 10LL;
            if (SetWaitableTimer(timer, &due_time, 0, nullptr, nullptr, FALSE))
                WaitForSingleObject(timer, INFINITE);
        } else if (remaining > min_sleep) {
            std::this_thread::sleep_until(target - spin_margin);
        }
        while (std::chrono::steady_clock::now() < target)
            SwitchToThread();
    }

private:
    using TimerPeriodFn = unsigned int(__stdcall *)(unsigned int);
    HMODULE winmm = nullptr;
    TimerPeriodFn time_begin_period = nullptr;
    TimerPeriodFn time_end_period = nullptr;
    bool period_active = false;
    HANDLE timer = nullptr;
#else
    void sleep_until(std::chrono::steady_clock::time_point target) const {
        std::this_thread::sleep_until(target);
    }
#endif
};

} // namespace

static void vblank_sync_thread(EmuEnvState &emuenv) {
    util::raise_thread_qos();
    DisplayState &display = emuenv.display;
    VblankPacer pacer;
    int prev_rate = 0;
    std::chrono::steady_clock::time_point next_vblank{};

    while (!display.abort.load()) {
        const int vblank_rate = std::max(1, display.vblank_rate.load(std::memory_order_relaxed));
        {
            const std::lock_guard<std::mutex> guard(display.mutex);

            {
                const std::lock_guard<std::mutex> guard_info(display.display_info_mutex);
                ++display.vblank_count;

                // in this case, even though no new game frames are being rendered, we still need to update the screen
                if (emuenv.kernel.is_threads_paused() || (emuenv.common_dialog.status == SCE_COMMON_DIALOG_STATUS_RUNNING)) {
                    // only display the UI/common dialog at ~30 fps
                    // this is necessary so that the command buffer processing doesn't get starved
                    // with vsync enabled and a screen with a refresh rate of 60Hz or less
                    const uint64_t ui_divider = static_cast<uint64_t>(std::max(1, vblank_rate / 30));
                    if (display.vblank_count % ui_divider == 0)
                        emuenv.renderer->should_display = true;
                }
            }

            // maybe we should also use a mutex for this part, but it shouldn't be an issue
            touch_vsync_update(emuenv);
            refresh_motion(emuenv.motion, emuenv.ctrl);

            // Notify Vblank callback in each VBLANK start
            for (auto &[_, cb] : display.vblank_callbacks)
                cb->event_notify(cb->get_notifier_id());

            display.vblank_waiters.wake_if([&](auto &waiter) {
                return waiter.entry.target_vcount <= display.vblank_count;
            });
        }
        const auto period = std::chrono::microseconds(1000000LL / vblank_rate);
        const auto now = std::chrono::steady_clock::now();
        if (vblank_rate != prev_rate) {
            next_vblank = now + period;
            prev_rate = vblank_rate;
        } else {
            next_vblank += period;
            if (now > next_vblank + period * 2)
                next_vblank = now + period;
        }
        pacer.sleep_until(next_vblank);
    }
}

void start_sync_thread(EmuEnvState &emuenv) {
    emuenv.display.vblank_thread = std::make_unique<std::thread>(vblank_sync_thread, std::ref(emuenv));
}

void wait_vblank(DisplayState &display, const ThreadStatePtr &wait_thread, const uint64_t target_vcount, const bool is_cb) {
    if (!wait_thread) {
        return;
    }

    std::unique_lock<std::mutex> lock(display.mutex);

    if (target_vcount <= display.vblank_count)
        return;

    // Nothing runs after the wait, so a thread exiting during it needs no handling here
    (void)display.vblank_waiters.wait(lock, wait_thread, { SCE_KERNEL_WAITTYPE_EVENT }, { target_vcount }, Deadline::max(), is_cb);
}

static void reset_swapchain_cycle(DisplayState &display, Address sync_object) {
    display.predicted_frames.resize(1);
    display.predicted_frames[0].sync_object = sync_object;
    display.predicted_frame_position = 0;
    display.predicted_cycles_seen = 0;
}

DisplayFrameInfo *predict_next_image(EmuEnvState &emuenv, Address sync_object) {
    auto &display = emuenv.display;
    std::lock_guard<std::mutex> lock(display.display_info_mutex);

    if (display.predicted_cycles_seen >= predict_threshold) {
        // just check that the next sync_object in line is the one we expect
        display.predicted_frame_position = (display.predicted_frame_position + 1) % display.predicted_frames.size();
        if (display.predicted_frames[display.predicted_frame_position].sync_object != sync_object)
            // bad, this isn't what we expect
            reset_swapchain_cycle(display, sync_object);

    } else if (display.predicted_cycles_seen >= 1) {
        display.predicted_frame_position = (display.predicted_frame_position + 1) % display.predicted_frames.size();

        if (display.predicted_frame_position == 0)
            display.predicted_cycles_seen++;

        if (display.predicted_frames[display.predicted_frame_position].sync_object != sync_object)
            // bad, this isn't what we expect
            reset_swapchain_cycle(display, sync_object);
    } else {
        // check if we have a cycle
        bool has_cycle = false;
        for (int idx = 0; idx < display.predicted_frames.size(); idx++) {
            if (display.predicted_frames[idx].sync_object == sync_object) {
                // we found a cycle
                has_cycle = true;
                display.predicted_frames.erase(display.predicted_frames.begin(), display.predicted_frames.begin() + idx);
                display.predicted_frame_position = 0;
                display.predicted_cycles_seen = 1;
                break;
            }
        }

        if (!has_cycle) {
            // predicted_frame_position is initialized to -1, so this is fine
            display.predicted_frame_position++;
            if (display.predicted_frame_position == display.predicted_frames.size()) {
                // keep the last max_expected_swapchain_size frames for the swapchain cycle
                if (display.predicted_frames.size() == max_expected_swapchain_size) {
                    display.predicted_frame_position = 0;
                } else {
                    display.predicted_frames.resize(display.predicted_frames.size() + 1);
                }
            }
            display.predicted_frames[display.predicted_frame_position].sync_object = sync_object;
        }
    }

    bool predict = display.predicted_cycles_seen >= predict_threshold;
    DisplayFrameInfo *frame = nullptr;
    if (predict) {
        // set the next framebuffer image here
        frame = new DisplayFrameInfo;
        *frame = display.predicted_frames[display.predicted_frame_position].frame_info;
    }

    return frame;
}

void update_prediction(EmuEnvState &emuenv, DisplayFrameInfo &frame) {
    auto &display = emuenv.display;
    std::lock_guard<std::mutex> lock(display.display_info_mutex);
    Address sync_object = display.current_sync_object;

    if (!display.predicting) {
        display.next_rendered_frame = frame;
        emuenv.renderer->should_display = true;
    }

    for (auto &pred_frame : display.predicted_frames) {
        if (pred_frame.sync_object != sync_object)
            continue;

        if (memcmp(&pred_frame.frame_info, &frame, sizeof(DisplayFrameInfo)) == 0)
            // we got what we expected, fine
            return;

        pred_frame.frame_info = frame;
        break;
    }

    if (display.predicting) {
        LOG_TRACE("Mispredicted the next swapchain image");
        display.next_rendered_frame = frame;
        emuenv.renderer->should_display = true;
    }

    // let predict_next_image reset the cycle if necessary
    display.predicted_cycles_seen = std::min(display.predicted_cycles_seen, 1U);
}

void DisplayState::deinit() {
    abort = true;
    if (vblank_thread && vblank_thread->joinable())
        vblank_thread->join();

    vblank_thread.reset();
    abort = false;

    {
        const std::lock_guard<std::mutex> guard(mutex);
        vblank_callbacks.clear();
    }

    {
        const std::lock_guard<std::mutex> guard(display_info_mutex);
        sce_frame = {};
        next_rendered_frame = {};
    }

    predicted_frames.clear();
    predicted_frame_position = static_cast<uint32_t>(-1);
    predicted_cycles_seen = 0;
    predicting = false;
    current_sync_object = 0;

    vblank_count = 0;
    last_setframe_vblank_count = 0;
    vblank_rate.store(DEFAULT_VBLANK_RATE, std::memory_order_relaxed);

    fps_hack = false;
    // pretty sure we set this on game boot
    fullscreen = false;
}