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
#include <mem/functions.h>
#include <mem/ptr.h>
#include <util/log.h>

#include <cstring>

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
                          "; The finger goes to the first eye wherever the game draws it (the point below is not used),\n"
                          "; and only while an eye is shown.\n"
                          "caution = 896, 260\ncaution_key = F\ncaution_gamepad = righttrigger\n"),
        },
    };
}

// The caution eyes. The HUD manager (*0x8146c308) keeps its widgets in a vector at +0x10..+0x14;
// the caution widget has id 4 (+4). It holds up to 8 eyes, one per alerting area, stacked down
// the right edge: [+0x4c] of them, layout element i at [+0x2c + 4i]. A new touch is tested against
// each eye in turn (0x8115849e: element 0x8115f51c, its parts 0x8115dc1e), and the first eye hit
// becomes the lock-on target. An element: position +0x24/+0x28 and scale +0x30/+0x34 (floats),
// shown when +0x7c and +0x44 (alpha) are non-zero, parts in a vector at +0x18..+0x1c. A part: x, y,
// width, height +0x24..+0x30 (int), offset +0x34/+0x38, scale +0x40/+0x44, pivot +0x70/+0x74
// (floats), shown when +0x8c and +0x54 are non-zero. Returns the centre of the first shown eye in
// screen pixels.
std::optional<std::pair<float, float>> caution_eye(EmuEnvState &emuenv) {
    const MemState &mem = emuenv.mem;
    const auto u32 = [&](Address a) -> std::optional<uint32_t> {
        if (!a || !is_valid_addr(mem, a) || !is_valid_addr(mem, a + 3))
            return std::nullopt;
        uint32_t v;
        std::memcpy(&v, Ptr<uint8_t>(a).get(mem), 4);
        return v;
    };
    const auto f32 = [&](Address a) -> float {
        const auto v = u32(a);
        float f = 0;
        if (v)
            std::memcpy(&f, &*v, 4);
        return f;
    };
    const auto mgr = u32(0x8146c308);
    if (!mgr || !*mgr)
        return std::nullopt;
    const auto begin = u32(*mgr + 0x10), end = u32(*mgr + 0x14);
    if (!begin || !end || *end < *begin || *end - *begin > 0x400)
        return std::nullopt;
    Address widget = 0;
    for (Address p = *begin; p < *end; p += 4) {
        const auto w = u32(p);
        if (w && *w && u32(*w + 4).value_or(0) == 4) {
            widget = *w;
            break;
        }
    }
    if (!widget)
        return std::nullopt;
    const uint32_t count = std::min<uint32_t>(u32(widget + 0x4c).value_or(0), 8);
    for (uint32_t i = 0; i < count; i++) {
        const auto e = u32(widget + 0x2c + 4 * i);
        if (!e || !*e)
            continue;
        const auto shown = u32(*e + 0x7c);
        if (!shown || !(*shown & 0xff) || f32(*e + 0x44) == 0)
            continue;
        const float ex = f32(*e + 0x24), ey = f32(*e + 0x28), esx = f32(*e + 0x30), esy = f32(*e + 0x34);
        const auto pb = u32(*e + 0x18), pe = u32(*e + 0x1c);
        if (!pb || !pe || *pe < *pb || *pe - *pb > 0x100)
            continue;
        // the union of the shown parts
        float l = 1e9f, t = 1e9f, r = -1e9f, b = -1e9f;
        for (Address p = *pb; p < *pe; p += 4) {
            const auto c = u32(p);
            if (!c || !*c || !(u32(*c + 0x8c).value_or(0) & 0xff) || f32(*c + 0x54) == 0)
                continue;
            const int32_t x = static_cast<int32_t>(u32(*c + 0x24).value_or(0)), y = static_cast<int32_t>(u32(*c + 0x28).value_or(0));
            const int32_t w = static_cast<int32_t>(u32(*c + 0x2c).value_or(0)), h = static_cast<int32_t>(u32(*c + 0x30).value_or(0));
            if (w <= 0 || h <= 0)
                continue;
            const float left = x + f32(*c + 0x34) + (x - f32(*c + 0x70)) * (f32(*c + 0x40) - 1.0f);
            const float top = y + f32(*c + 0x38) + (y - f32(*c + 0x74)) * (f32(*c + 0x44) - 1.0f);
            l = std::min(l, left);
            t = std::min(t, top);
            r = std::max(r, left + w);
            b = std::max(b, top + h);
        }
        if (r < l)
            continue;
        const float sx = ex + (l + r) / 2 * esx, sy = ey + (t + b) / 2 * esy;
        if (sx < 0 || sx >= 960 || sy < 0 || sy >= 544)
            continue;
        return std::make_pair(sx, sy);
    }
    return std::nullopt;
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
