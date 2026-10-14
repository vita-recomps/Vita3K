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
#include "saves.h"

#include <display/state.h>
#include <emuenv/state.h>
#include <mem/functions.h>
#include <mem/ptr.h>
#include <ngs/state.h>
#include <renderer/state.h>
#include <util/log.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <chrono>
#include <mutex>
#include <optional>
#include <vector>

// provides WinMain for the windowed executable, which calls main below
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_messagebox.h>

namespace {

namespace layout = game_frontend::layout;
using Kind = game_frontend::SettingSpec::Kind;

constexpr const char *TITLE_ID = "PCSE00015";

// [audio]: the shared volume, and the sound effects Vita3K long left out (ngs::State::Effects),
// on for this game: its reverb buss (filled from the reverb's preset 0) and the fade-outs its
// sound manager asks for when it stops a sound
game_frontend::IniSection audio_layout() {
    game_frontend::IniSection section = layout::audio();
    section.settings.push_back({ "effects", Kind::Bool, "true", 0, 0, {},
        "Reverb and fade-outs as the game asks for them (false: the dry sound of earlier builds,\n"
        "for comparison). Read when the game starts." });
    return section;
}

game_frontend::IniLayout ini_layout() {
    const std::string id = "shinobido2", exe = "shinobido2.exe";
    return {
        "shinobido2 settings. Lines starting with ; are comments; delete the ; to use a line.",
        {
            layout::paths(TITLE_ID, id, exe),
            { "display", "",
                { layout::scale(), layout::window_scale(), layout::fullscreen(id, exe), layout::vsync(),
                    { "frame_rate", Kind::Choice, "30", 0, 0, { "30", "60" },
                        "The game's frame rate: 30 as on the Vita, or 60 (experimental: menus and HUD\n"
                        "animate twice as fast). Read when the game starts." },
                    layout::filter() } },
            audio_layout(),
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

// The frame rate (docs/03-plan.md, 60 fps). main (0x81000016) creates the application (its
// pointer at 0x8146c2b0) and sets its frame rate [app+8] to 30; the run loop (0x8114d024) reads
// it once, on entry, for both the frame period and the minimum vblanks between frames, and keeps
// them in registers. So 60 is written before the run loop starts, while the game initialises
// (init takes far longer than a front-end tick: GXM setup and the first loads); the loop stores
// the vcount at [app+0x1d8] on entry, which the constructor zeroed, so a non-zero value there
// means it is too late and the rate is left at 30.
// The game's time module (0x81054b6e, every frame) measures the frame time (an average over 8
// frames, at most [0x81468998]) and stores the step every update uses at 0x81468978, unless it is
// in fixed-step mode ([0x81538370] != 0), where the step is [0x81468998] itself, 1/30 s.
// Application init and scripted scenes select fixed mode, and play can stay in it, so a fixed
// 1/30 step becomes 1/60 at 60 fps. But some scripted scenes step per frame elsewhere too: two
// mid-mission story scenes ran twice as fast at 60 with the step made 1/60 (owner, 2026-10-14).
// So scripted scenes run at 30 fps as on the Vita, with the 1/30 step: the run loop waits for one
// vblank per frame (sceDisplayWaitVblankStartCB), which then waits for two
// (DisplayState::vblank_start_count). Which state is a scene came from the owner's tests of the
// last missions' boss fights (2026-10-14), with these signals logged:
// - the script controller (*0x8146ac74) holds an event scene at +0x10 (0x8112e4fc builds one):
//   0 in play, 1 in a scene, but also through a boss fight, which is part of its event;
// - the scene flag [0x815399f8]: set when the controller's scene start freezes the player (2),
//   cleared by its end; it stays set through a boss fight after its intro, and is 0 in the second
//   boss's intro, which follows a load;
// - the HUD's shown byte (+0x1c of *0x8146c308): 0 in most scenes, 1 in play and boss fights, but
//   also 1 through the second boss's intro.
// Scenes with the scene flag set (opening scenes, boss intros and outros) play right at 60 with
// the 1/60 step (the owner's playthrough); the two that ran twice as fast were the event scenes
// after a load, without the scene flag. So only an event scene without the scene flag runs at 30
// (the owner's suggestion, checked on the second-to-last mission); that is also the second or two
// while any scene loads, before its flag is set. Outside missions there is no script controller
// (the title, its menus and the hub): those run at 30 too, where menu animations step by a literal
// 1/30 (the owner's request). In play, boss fights and other scenes, 60 fps.
struct FrameRate {
    enum class Boot { Waiting, Set, TooLate };
    Boot boot = Boot::Waiting;
    int scene = -1; // a scripted scene at 30 fps, as last set (-1: not yet)
    std::array<int64_t, 4> signals{ -9, -9, -9, -9 }; // as last logged
    int logged = 0; // logged signal changes
};

// a byte or a word of guest memory, or -1 when the address is not mapped
int64_t read_guest(EmuEnvState &emuenv, Address address, int size) {
    if (!address || !is_valid_addr(emuenv.mem, address) || !is_valid_addr(emuenv.mem, address + size - 1))
        return -1;
    uint32_t v = 0;
    std::memcpy(&v, Ptr<uint8_t>(address).get(emuenv.mem), size);
    return v;
}

void update_frame_rate(EmuEnvState &emuenv, FrameRate &state) {
    auto &mem = emuenv.mem;
    const auto ptr = [&](Address a, uint32_t size) -> uint8_t * { return is_valid_addr(mem, a) && is_valid_addr(mem, a + size - 1) ? Ptr<uint8_t>(a).get(mem) : nullptr; };
    const auto read32 = [](const uint8_t *p) { uint32_t v; std::memcpy(&v, p, 4); return v; };
    if (state.boot == FrameRate::Boot::Waiting) {
        const uint8_t *const holder = ptr(0x8146c2b0, 4);
        const uint32_t app_addr = holder ? read32(holder) : 0;
        uint8_t *const app = app_addr ? ptr(app_addr, 0x1e0) : nullptr;
        if (!app || read32(app + 8) != 30)
            return;
        if (read32(app + 0x1d8) != 0) {
            state.boot = FrameRate::Boot::TooLate;
            LOG_WARN("frame_rate: the game's run loop had already started; it stays at 30 fps");
            return;
        }
        const uint32_t sixty = 60;
        std::memcpy(app + 8, &sixty, 4);
        state.boot = FrameRate::Boot::Set;
        LOG_INFO("frame_rate: 60 fps (application at {:#x})", app_addr);
    }
    if (state.boot != FrameRate::Boot::Set)
        return;
    const uint8_t *const fixed_flag = ptr(0x81538370, 1);
    uint8_t *const step = ptr(0x81468998, 8);
    if (!fixed_flag || !step)
        return;
    const int64_t controller = read_guest(emuenv, 0x8146ac74, 4);
    const int64_t hud = read_guest(emuenv, 0x8146c308, 4);
    const std::array<int64_t, 4> signals = {
        controller > 0 ? (read_guest(emuenv, static_cast<Address>(controller) + 0x10, 4) > 0 ? 1 : 0) : -1, // event scene
        controller > 0 ? read_guest(emuenv, static_cast<Address>(controller) + 0x58, 4) : -1, // controller state
        read_guest(emuenv, 0x815399f8, 1), // scene flag
        hud > 0 ? read_guest(emuenv, static_cast<Address>(hud) + 0x1c, 1) : -1, // HUD shown
    };
    if (signals != state.signals && state.logged++ < 400)
        LOG_INFO("frame_rate: event scene {}, controller state {}, scene flag {}, HUD shown {}", signals[0], signals[1], signals[2], signals[3]);
    state.signals = signals;
    const bool menus = controller <= 0;
    const int scene = (signals[0] == 1 && signals[2] == 0) || menus ? 1 : 0;
    if (*fixed_flag) {
        // the fixed step: 1/60 in play at 60 fps, 1/30 in a scene at 30 (only values we know)
        const double from = static_cast<double>(scene ? 1.0f / 60.0f : 1.0f / 30.0f);
        const double to = static_cast<double>(scene ? 1.0f / 30.0f : 1.0f / 60.0f);
        double value;
        std::memcpy(&value, step, 8);
        if (value == from)
            std::memcpy(step, &to, 8);
    }
    if (scene == state.scene)
        return;
    state.scene = scene;
    emuenv.display.vblank_start_count = scene ? 2 : 1;
    LOG_INFO("frame_rate: {}", !scene ? "60 fps" : menus ? "menus (no mission): 30 fps" : "an event scene after a load: 30 fps");
}

// --import-save / --export-save
struct SaveCommand {
    std::string import_from, export_to;
    int slot = -1; // -1: every slot
    bool quiet = false;
};

} // namespace

int main(int argc, char *argv[]) {
    // the game's own command line, besides the frontend's (game-frontend/frontend.cpp):
    //   --import-save <folder|zip> copy saves in (decrypted, e.g. from a Vita), keeping the old ones
    //   --export-save <folder>     copy saves out
    //   --slot <0-9>               one save slot for the two above (default: every slot)
    //   --quiet                    no message box for the two above (the result is in the log)
    SaveCommand save;
    game_frontend::Profile profile;
    profile.title_id = TITLE_ID;
    profile.name = "Shinobido 2: Revenge of Zen";
    profile.id = "shinobido2";
    profile.ini = ini_layout();
    profile.default_touches = { { "caution", 896, 260, "F", "righttrigger" } };
    profile.touch_targets["caution"] = caution_eye;
    profile.hooks.parse_arg = [&](game_frontend::Context &, const std::vector<std::string> &args, size_t &i) {
        const std::string &arg = args[i];
        const bool has_value = i + 1 < args.size();
        if (arg == "--import-save" && has_value)
            save.import_from = args[++i];
        else if (arg == "--export-save" && has_value)
            save.export_to = args[++i];
        else if (arg == "--slot" && has_value)
            save.slot = std::atoi(args[++i].c_str());
        else if (arg == "--quiet")
            save.quiet = true;
        else
            return false;
        return true;
    };
    profile.hooks.before_init = [&](game_frontend::Context &ctx) -> std::optional<int> {
        if (save.import_from.empty() && save.export_to.empty())
            return std::nullopt;
        const auto &vita_fs = ctx.settings->vita_fs;
        std::string message;
        const bool ok = !save.import_from.empty()
            ? shinobido2::import_save(vita_fs, fs_utils::utf8_to_path(save.import_from), save.slot, ctx.data_dir / "save-backups", message)
            : shinobido2::export_save(vita_fs, fs_utils::utf8_to_path(save.export_to), save.slot, message);
        if (ok)
            LOG_INFO("Save: {}", message);
        else
            LOG_ERROR("Save: {}", message);
        if (!save.quiet)
            SDL_ShowSimpleMessageBox(ok ? SDL_MESSAGEBOX_INFORMATION : SDL_MESSAGEBOX_ERROR, ctx.window_name.c_str(),
                ("Save: " + message).c_str(), nullptr);
        return ok ? 0 : 1;
    };
    profile.hooks.before_run = [](game_frontend::Context &ctx) {
        const bool effects = ctx.settings->values.get_bool("audio.effects");
        ctx.emuenv->ngs.effects.reverb = effects;
        ctx.emuenv->ngs.effects.release = effects;
        LOG_INFO("audio effects (reverb, fade-outs): {}", effects ? "on" : "off");
    };
    FrameRate frame_rate;
    profile.hooks.on_tick = [&](game_frontend::Context &ctx) {
        if (ctx.settings->values.get_int("display.frame_rate") == 60)
            update_frame_rate(*ctx.emuenv, frame_rate);
    };
    return game_frontend::run(profile, argc, argv);
}
