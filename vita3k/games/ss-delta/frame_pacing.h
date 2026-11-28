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

#pragma once

#include <cstdint>

namespace ss_delta {

struct FramePacing {
    int fps;
    uint64_t period_us;
    uint32_t flip_threshold;
    int vblank_rate;
};

// Supported frame rates and their pacing parameters:
// - 30 fps: Vita default (33333 us timer, flip every 2nd 60 Hz vblank).
// - 60 fps: 16000 us timer (666 us margin below 16666 us for host sleep overshoot), every 60 Hz vblank.
// - 90 fps: 9800 us timer (1311 us margin below 11111 us for Windows 1 ms CV rounding), every 90 Hz vblank.
// - 120 fps: 6800 us timer (1533 us margin below 8333 us for Windows 1 ms CV rounding), every 120 Hz vblank.
// Because blocking pad/touch reads wake once per emulated vblank, the game cannot run faster than
// vblank_rate even with the timer margin.
inline constexpr FramePacing FRAME_PACING_TABLE[] = {
    { 30, 33333, 2, 60 },
    { 60, 16000, 1, 60 },
    { 90, 9800, 1, 90 },
    { 120, 6800, 1, 120 },
};

inline const FramePacing *frame_pacing_for(int fps) {
    for (const auto &entry : FRAME_PACING_TABLE)
        if (entry.fps == fps)
            return &entry;
    return nullptr;
}

inline bool is_known_frame_pacing(uint64_t period_us, uint32_t flip_threshold) {
    for (const auto &entry : FRAME_PACING_TABLE)
        if (entry.period_us == period_us && entry.flip_threshold == flip_threshold)
            return true;
    return false;
}

struct FramePacingUpdate {
    int rate = 0; // applied rate, or -1 if guest memory held unexpected values or target is invalid
    bool changed = false;
    bool unexpected = false;
    uint64_t period_us = 0;
    uint32_t flip_threshold = 0;
    int vblank_rate = 60;
};

// Validates the current guest timer period & flip threshold against all known states, handles the
// legacy loading-screen 30 fps fallback, and updates period/flip/vblank_rate when needed.
inline FramePacingUpdate apply_frame_pacing(int target_fps, bool loading_fallback,
    uint64_t &period_us, uint32_t &flip_threshold, int &vblank_rate) {
    const FramePacing *const target = frame_pacing_for(loading_fallback ? 30 : target_fps);
    if (!target)
        return { -1, false, true, period_us, flip_threshold, vblank_rate };

    if (period_us == target->period_us && flip_threshold == target->flip_threshold && vblank_rate == target->vblank_rate)
        return { target->fps, false, false, period_us, flip_threshold, vblank_rate };

    if (!is_known_frame_pacing(period_us, flip_threshold))
        return { -1, false, true, period_us, flip_threshold, vblank_rate };

    period_us = target->period_us;
    flip_threshold = target->flip_threshold;
    vblank_rate = target->vblank_rate;
    return { target->fps, true, false, period_us, flip_threshold, vblank_rate };
}

} // namespace ss_delta
