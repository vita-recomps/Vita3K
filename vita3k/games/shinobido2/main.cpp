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

// shinobido2.exe: Shinobido 2: Revenge of Zen (US, PCSE00015) on the shared single-game frontend
// (game-frontend/profile.h). The utakata project's docs describe the port.

#include "../../game-frontend/profile.h"

#include <emuenv/state.h>
#include <renderer/state.h>
#include <util/log.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <chrono>
#include <mutex>
#include <optional>
#include <vector>

// provides WinMain for the windowed executable, which calls main below
#include <SDL3/SDL_main.h>

namespace {

namespace layout = game_frontend::layout;

constexpr const char *TITLE_ID = "PCSE00015";

game_frontend::IniLayout ini_layout() {
    const std::string id = "shinobido2", exe = "shinobido2.exe";
    return {
        "shinobido2 settings. Lines starting with ; are comments; delete the ; to use a line.",
        {
            layout::paths(TITLE_ID, id, exe),
            { "display", "", { layout::scale(), layout::window_scale(), layout::fullscreen(id, exe), layout::vsync(), layout::filter() } },
            layout::audio(),
            layout::recompiled(id),
            layout::debug(id),
            layout::keyboard(),
            layout::gamepad(),
            layout::touch("; Caution (the eye on the right in a mission, one per nearby enemy): focus on that enemy.\n"
                          "; The finger goes to the topmost eye wherever it is (the point below is not used),\n"
                          "; and only while an eye is shown.\n"
                          "caution = 896, 260\ncaution_key = F\ncaution_gamepad = righttrigger\n"),
        },
    };
}

// The caution eyes: one per nearby enemy, stacked down the right edge of the screen (their
// centres at x = 896 of 960, the first at y = 258). The iris takes the enemy's state (white: not
// engaged, yellow: spotted, red: combat, purple: cautious); the frame around it (the silver wings,
// the rim, the pupil) does not, so the front end looks for the frame. The game decides which eye a
// touch hits from its HUD layout, which it rebuilds every frame; the front end finds the eyes in the
// picture instead. When the binding goes down, one frame is read back, and the finger is held on
// the topmost eye for as long as the binding is held; with no eye in that frame, no finger goes
// down. Checked on 116 screenshots (white and red eyes, one fading in, one over a bright wall):
// eyes score at most 21, everything else at least 43.
struct CautionTarget {
    std::mutex mutex;
    bool requested = false;
    std::optional<std::pair<float, float>> point;
    std::chrono::steady_clock::time_point last_call{};
};

// the frame's pixels on the 960x544 grid, relative to the eye's centre, every other one: dx, dy, r, g, b
struct EyePixel {
    int8_t dx, dy;
    uint8_t r, g, b;
};
constexpr EyePixel EYE_FRAME[] = {
    {-38, -16, 156, 153, 154}, {-38, -14, 159, 156, 156}, {-36, -14, 157, 154, 155},
    {-24, -14, 203, 201, 200}, {-38, -12, 159, 156, 156}, {-36, -12, 159, 156, 157},
    {-26, -12, 209, 204, 207}, {-24, -12, 85, 85, 83}, {24, -12, 160, 159, 159},
    {28, -12, 186, 181, 184}, {-40, -10, 160, 156, 157}, {-38, -10, 160, 157, 158},
    {-28, -10, 106, 106, 105}, {-26, -10, 104, 105, 103}, {-24, -10, 201, 200, 200},
    {24, -10, 188, 185, 187}, {26, -10, 187, 187, 188}, {30, -10, 200, 199, 201},
    {-32, -8, 191, 189, 189}, {-28, -8, 157, 156, 156}, {-26, -8, 162, 162, 161},
    {-24, -8, 204, 201, 201}, {26, -8, 190, 190, 192}, {28, -8, 156, 156, 157},
    {-32, -6, 33, 31, 31}, {-30, -6, 143, 143, 142}, {-28, -6, 150, 149, 149},
    {-26, -6, 197, 195, 195}, {-24, -6, 205, 189, 185}, {26, -6, 199, 198, 200},
    {28, -6, 146, 147, 148}, {30, -6, 145, 146, 147}, {32, -6, 100, 98, 101}, {34, -6, 67, 66, 68},
    {-34, -4, 80, 79, 78}, {-32, -4, 106, 105, 103}, {-30, -4, 32, 31, 31},
    {-28, -4, 148, 147, 147}, {-26, -4, 197, 196, 196}, {26, -4, 200, 198, 200},
    {28, -4, 147, 147, 148}, {30, -4, 94, 95, 95}, {32, -4, 29, 29, 31}, {34, -4, 139, 136, 139},
    {38, -4, 191, 190, 193}, {-40, -2, 173, 168, 170}, {-38, -2, 91, 88, 90},
    {-36, -2, 103, 105, 102}, {-34, -2, 147, 149, 145}, {-32, -2, 155, 153, 151},
    {-30, -2, 117, 117, 115}, {-28, -2, 38, 36, 36}, {-26, -2, 181, 180, 179}, {0, -2, 26, 19, 20},
    {2, -2, 30, 21, 22}, {26, -2, 192, 189, 191}, {28, -2, 83, 83, 85}, {30, -2, 37, 36, 38},
    {32, -2, 148, 149, 150}, {34, -2, 150, 150, 151}, {36, -2, 142, 146, 144},
    {-38, 0, 146, 144, 144}, {-36, 0, 146, 149, 145}, {-34, 0, 147, 150, 145},
    {-32, 0, 144, 143, 141}, {-30, 0, 141, 141, 140}, {-28, 0, 132, 130, 130}, {-26, 0, 88, 87, 86},
    {-2, 0, 43, 25, 25}, {0, 0, 16, 15, 17}, {2, 0, 18, 16, 18}, {28, 0, 81, 81, 82},
    {30, 0, 132, 132, 134}, {32, 0, 139, 139, 141}, {34, 0, 140, 139, 141}, {36, 0, 143, 147, 145},
    {38, 0, 142, 146, 144}, {-40, 2, 90, 86, 89}, {-38, 2, 87, 83, 85}, {-36, 2, 143, 144, 142},
    {-34, 2, 143, 143, 141}, {-32, 2, 141, 140, 138}, {-30, 2, 138, 139, 137}, {-28, 2, 82, 82, 80},
    {-26, 2, 130, 129, 128}, {-2, 2, 44, 24, 25}, {0, 2, 15, 14, 16}, {2, 2, 17, 15, 17},
    {26, 2, 192, 192, 191}, {30, 2, 123, 122, 125}, {32, 2, 135, 136, 136}, {34, 2, 135, 136, 136},
    {36, 2, 138, 139, 140}, {38, 2, 135, 134, 136}, {-36, 4, 31, 27, 30}, {-34, 4, 139, 140, 138},
    {-32, 4, 139, 139, 136}, {-30, 4, 25, 24, 24}, {-28, 4, 38, 40, 37}, {-26, 4, 177, 173, 175},
    {26, 4, 196, 196, 195}, {28, 4, 124, 122, 125}, {30, 4, 31, 31, 33}, {32, 4, 94, 95, 95},
    {34, 4, 134, 136, 136}, {36, 4, 80, 80, 81}, {-36, 6, 207, 206, 206}, {-34, 6, 40, 38, 37},
    {-32, 6, 29, 25, 27}, {-30, 6, 88, 84, 87}, {-28, 6, 149, 147, 149}, {-26, 6, 181, 178, 180},
    {26, 6, 197, 199, 197}, {28, 6, 148, 147, 149}, {30, 6, 144, 143, 145}, {32, 6, 19, 22, 21},
    {34, 6, 20, 22, 21}, {36, 6, 92, 91, 93}, {-34, 8, 194, 192, 191}, {-30, 8, 89, 85, 88},
    {-28, 8, 151, 149, 150}, {-26, 8, 182, 179, 181}, {-24, 8, 203, 195, 195},
    {26, 8, 197, 199, 198}, {28, 8, 147, 146, 148}, {30, 8, 144, 143, 145}, {32, 8, 21, 23, 22},
    {-40, 10, 163, 159, 160}, {-32, 10, 171, 168, 166}, {-30, 10, 128, 128, 125},
    {-28, 10, 51, 51, 48}, {-26, 10, 157, 156, 154}, {-24, 10, 201, 196, 198},
    {24, 10, 201, 177, 180}, {26, 10, 197, 193, 198}, {28, 10, 153, 153, 154}, {30, 10, 54, 55, 54},
    {32, 10, 200, 199, 201}, {-28, 12, 202, 201, 201}, {-24, 12, 156, 154, 154},
    {24, 12, 194, 190, 195}, {26, 12, 150, 149, 151}, {30, 12, 183, 182, 185},
    {-26, 14, 196, 195, 194}, {24, 14, 123, 122, 124}, {26, 14, 159, 159, 159},
    {-40, 16, 163, 160, 161}, {-24, 16, 178, 177, 173}, {24, 16, 197, 196, 198},
    {-40, 18, 162, 159, 159},
};

// the mean difference per channel between the frame and the picture around (896, y)
float eye_score(const std::vector<uint32_t> &frame, uint32_t width, uint32_t height, int y) {
    int total = 0;
    for (const EyePixel &e : EYE_FRAME) {
        // pixels are 0xAABBGGRR
        const uint32_t px = frame[static_cast<size_t>((y + e.dy) * height / 544) * width + static_cast<size_t>((896 + e.dx) * width / 960)];
        total += std::abs(static_cast<int>(px & 0xff) - e.r) + std::abs(static_cast<int>((px >> 8) & 0xff) - e.g)
            + std::abs(static_cast<int>((px >> 16) & 0xff) - e.b);
    }
    return total / (3.0f * std::size(EYE_FRAME));
}

std::optional<std::pair<float, float>> find_caution_eye(const std::vector<uint32_t> &frame, uint32_t width, uint32_t height) {
    if (frame.size() != static_cast<size_t>(width) * height || width < 960 || height < 544)
        return std::nullopt;
    constexpr float THRESHOLD = 30; // between the eyes' 21 and the rest's 43
    // the first y from the top that matches, then the best match just below it: the topmost eye
    for (int y = 100; y < 500; y++) {
        if (eye_score(frame, width, height, y) >= THRESHOLD)
            continue;
        int best = y;
        float best_score = eye_score(frame, width, height, y);
        for (int y2 = y + 1; y2 < std::min(y + 10, 500); y2++) {
            const float score = eye_score(frame, width, height, y2);
            if (score < best_score) {
                best = y2;
                best_score = score;
            }
        }
        return std::make_pair(896.0f, static_cast<float>(best));
    }
    return std::nullopt;
}

std::optional<std::pair<float, float>> caution_eye(EmuEnvState &emuenv) {
    static CautionTarget state;
    const auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(state.mutex);
        // a new press when the binding was not held in the last frames
        if (now - state.last_call > std::chrono::milliseconds(150)) {
            state.requested = false;
            state.point.reset();
        }
        state.last_call = now;
        if (state.requested || !emuenv.renderer)
            return state.point;
        state.requested = true;
    }
    {
        renderer::State *const renderer = emuenv.renderer.get();
        renderer->run_on_render_thread([renderer, &display = emuenv.display] {
            uint32_t width = 0, height = 0;
            const std::vector<uint32_t> frame = renderer->dump_frame(display, width, height);
            const auto eye = find_caution_eye(frame, width, height);
            std::lock_guard<std::mutex> lock(state.mutex);
            state.point = eye;
            LOG_INFO("caution: {}", eye ? fmt::format("eye at {:.0f}, {:.0f}", eye->first, eye->second) : std::string("no eye on screen"));
        });
    }
    std::lock_guard<std::mutex> lock(state.mutex);
    return state.point;
}

} // namespace

int main(int argc, char *argv[]) {
    game_frontend::Profile profile;
    profile.title_id = TITLE_ID;
    profile.name = "Shinobido 2: Revenge of Zen";
    profile.id = "shinobido2";
    profile.ini = ini_layout();
    profile.default_touches = { { "caution", 896, 260, "F", "righttrigger" } };
    profile.touch_targets["caution"] = caution_eye;
    return game_frontend::run(profile, argc, argv);
}
