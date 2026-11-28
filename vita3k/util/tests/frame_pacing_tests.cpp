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

#include "frame_pacing.h"

#include <cstdlib>
#include <iostream>

#define CHECK(expr)                                                                                  \
    do {                                                                                             \
        if (!(expr)) {                                                                               \
            std::cerr << "FAIL (" << __FILE__ << ":" << __LINE__ << "): " << #expr << std::endl;     \
            std::exit(1);                                                                            \
        }                                                                                            \
    } while (0)

namespace {

void test_all_live_transitions() {
    uint64_t period = 33333;
    uint32_t flip = 2;
    int vblank = 60;

    // Repeated live switches across every supported rate (including back to 30)
    const int rates[] = { 30, 60, 90, 120 };
    for (int from : rates) {
        auto init = ss_delta::apply_frame_pacing(from, false, period, flip, vblank);
        CHECK(!init.unexpected);
        CHECK(init.rate == from);
        for (int to : rates) {
            const auto *expected = ss_delta::frame_pacing_for(to);
            CHECK(expected != nullptr);
            auto res = ss_delta::apply_frame_pacing(to, false, period, flip, vblank);
            CHECK(!res.unexpected);
            CHECK(res.rate == to);
            CHECK(res.changed == (from != to));
            CHECK(period == expected->period_us);
            CHECK(flip == expected->flip_threshold);
            CHECK(vblank == expected->vblank_rate);

            // Second call at the same rate must be a no-op
            auto again = ss_delta::apply_frame_pacing(to, false, period, flip, vblank);
            CHECK(!again.unexpected);
            CHECK(!again.changed);
            CHECK(again.rate == to);

            // Return to `from` for the next pair
            auto back = ss_delta::apply_frame_pacing(from, false, period, flip, vblank);
            CHECK(!back.unexpected);
            CHECK(back.rate == from);
        }
    }
}

void test_loading_screen_fallback() {
    uint64_t period = 33333;
    uint32_t flip = 2;
    int vblank = 60;

    // Enter 120 fps, drop to 30 during legacy loading screen, then restore 120
    auto r120 = ss_delta::apply_frame_pacing(120, false, period, flip, vblank);
    CHECK(r120.changed && r120.rate == 120 && vblank == 120 && period == 6800 && flip == 1);

    auto load = ss_delta::apply_frame_pacing(120, true, period, flip, vblank);
    CHECK(load.changed && load.rate == 30 && vblank == 60 && period == 33333 && flip == 2);

    auto steady = ss_delta::apply_frame_pacing(120, true, period, flip, vblank);
    CHECK(!steady.changed && !steady.unexpected && steady.rate == 30);

    auto done = ss_delta::apply_frame_pacing(120, false, period, flip, vblank);
    CHECK(done.changed && done.rate == 120 && vblank == 120 && period == 6800 && flip == 1);

    // Same for 90 fps
    auto r90 = ss_delta::apply_frame_pacing(90, false, period, flip, vblank);
    CHECK(r90.changed && r90.rate == 90 && vblank == 90 && period == 9800 && flip == 1);

    auto load90 = ss_delta::apply_frame_pacing(90, true, period, flip, vblank);
    CHECK(load90.changed && load90.rate == 30 && vblank == 60 && period == 33333 && flip == 2);

    auto done90 = ss_delta::apply_frame_pacing(90, false, period, flip, vblank);
    CHECK(done90.changed && done90.rate == 90 && vblank == 90 && period == 9800 && flip == 1);
}

void test_unexpected_guest_state_and_invalid_targets() {
    // Corrupted period is left untouched
    uint64_t period = 12345;
    uint32_t flip = 2;
    int vblank = 60;
    auto bad_period = ss_delta::apply_frame_pacing(60, false, period, flip, vblank);
    CHECK(bad_period.unexpected && !bad_period.changed && bad_period.rate == -1);
    CHECK(period == 12345 && flip == 2 && vblank == 60);

    // Mismatched (period, flip) pair is left untouched
    period = 16000;
    flip = 2;
    vblank = 60;
    auto bad_pair = ss_delta::apply_frame_pacing(120, false, period, flip, vblank);
    CHECK(bad_pair.unexpected && !bad_pair.changed && bad_pair.rate == -1);
    CHECK(period == 16000 && flip == 2 && vblank == 60);

    // Unsupported target rate is rejected without touching guest state
    period = 33333;
    flip = 2;
    vblank = 60;
    for (int invalid : { 0, -30, 45, 144 }) {
        auto bad_target = ss_delta::apply_frame_pacing(invalid, false, period, flip, vblank);
        CHECK(bad_target.unexpected && !bad_target.changed && bad_target.rate == -1);
        CHECK(period == 33333 && flip == 2 && vblank == 60);
    }
}

} // namespace

int main() {
    test_all_live_transitions();
    test_loading_screen_fallback();
    test_unexpected_guest_state_and_invalid_targets();
    std::cout << "frame_pacing_tests: all checks passed" << std::endl;
    return 0;
}
