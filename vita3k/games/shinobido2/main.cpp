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

#include "../../game-frontend/button_labels.h"
#include "../../game-frontend/profile.h"
#include "saves.h"

#include <config/state.h>
#include <cpu/aot.h>
#include <ctrl/state.h>
#include <display/state.h>
#include <emuenv/state.h>
#include <mem/functions.h>
#include <mem/ptr.h>
#include <ngs/state.h>
#include <renderer/functions.h>
#include <renderer/state.h>
#include <renderer/texture_cache.h>
#include <util/log.h>
#include <util/system.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <chrono>
#include <map>
#include <mutex>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

// provides WinMain for the windowed executable, which calls main below
#include <SDL3/SDL_main.h>
#include <SDL3/SDL_messagebox.h>
#include <stb_image_write.h>

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
        "Reverb and fade-outs as the game asks for them (Off: the dry sound of earlier builds,\n"
        "for comparison). Read when the game starts." });
    section.settings.back().title = "Sound effects (reverb, fades)";
    return section;
}

// [game] language: the Vita's system language, which the game reads once at boot (0x8114ce00): French
// and Spanish give its languages 3 and 4, anything else English (1), kept at +0x1e0 of the
// application object (utakata docs/01-research.md, "Text language"). It has no language option of
// its own.
const std::map<std::string, SceSystemParamLang> LANGUAGES{
    { "english", SCE_SYSTEM_PARAM_LANG_ENGLISH_US },
    { "french", SCE_SYSTEM_PARAM_LANG_FRENCH },
    { "spanish", SCE_SYSTEM_PARAM_LANG_SPANISH },
};

game_frontend::IniSection game_layout() {
    game_frontend::SettingSpec language{ "language", Kind::Choice, "english", 0, 0, { "english", "french", "spanish" },
        "The game's language, also in the system dialogs: english, french or spanish. Read when the game\n"
        "starts." };
    language.title = "Language";
    language.choice_labels = { "English", "French", "Spanish" };
    game_frontend::SettingSpec skip_intro{ "skip_intro", Kind::Bool, "false", 0, 0, {},
        "Skip the company logos at start: the opening movie follows the loading screen at once (Start\n"
        "skips that, as on the Vita)." };
    skip_intro.title = "Skip intro logos";
    return { "game", "", { language, skip_intro, game_frontend::button_labels_setting(), layout::repatch() } };
}

// [cheats]: switched in the settings menu while the game runs (apply_cheats)
game_frontend::IniSection cheats_layout() {
    game_frontend::IniSection cheats{ "cheats",
        "Cheats, all off by default. They change the game's values while it runs; on the Cheats page of\n"
        "the settings menu they switch at once. Money and stats are kept when the game saves, so export\n"
        "your saves first if you want to go back (--export-save).",
        {
            { "infinite_health", Kind::Bool, "false", 0, 0, {},
                "In missions, your health is refilled to its maximum as soon as it drops. A hit that takes\n"
                "it all at once can still bring you down." },
            { "infinite_tools", Kind::Bool, "false", 0, 0, {},
                "In missions, ninja tools and items don't run out: each one used is given back at once." },
            { "one_hit_kills", Kind::Bool, "false", 0, 0, {},
                "In missions, every other character falls to one hit: their health is kept at 1. Allies too,\n"
                "in a mission that has them." },
            { "max_money", Kind::Bool, "false", 0, 0, {}, "Keep your money at 9,999,999." },
            { "max_stats", Kind::Bool, "false", 0, 0, {},
                "Raise both characters' level to 99, stamina to 200, offence, defence and zankoku to 100 and\n"
                "SP to 255 (the values of Apollo's save patches). Health follows stamina from the next\n"
                "mission." },
        } };
    for (auto &spec : cheats.settings)
        spec.live = true;
    cheats.settings[1].title = "Infinite ninja tools";
    cheats.settings[2].title = "One-hit kills";
    cheats.settings[3].title = "Max money";
    cheats.settings[4].title = "Max stats";
    return cheats;
}

game_frontend::IniLayout ini_layout() {
    const std::string id = "shinobido2", exe = id + game_frontend::executable_suffix;
    return {
        "shinobido2 settings. Lines starting with ; are comments; delete the ; to use a line.",
        {
            layout::paths(TITLE_ID, id, exe),
            { "display", "",
                { layout::scale(), layout::window_scale(), layout::fullscreen(id, exe), layout::vsync(),
                    { "frame_rate", Kind::Choice, "30", 0, 0, { "30", "60" },
                        "The game's frame rate: 30 as on the Vita, or 60 in missions (the title, menus, hub and\n"
                        "the scenes after a loading screen stay at 30; the pause menu and HUD animate twice as\n"
                        "fast). Read when the game starts." },
                    layout::filter(),
                    layout::fps_counter(),
                    layout::mipmaps(true), layout::custom_textures() } },
            audio_layout(),
            game_layout(),
            cheats_layout(),
            layout::recompiled(id),
            layout::debug(id),
            layout::keyboard(),
            layout::gamepad(),
#ifndef __ANDROID__
            // the mouse camera: the right stick turns the camera in the field (a PC's mouse)
            layout::mouse(),
#endif
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

// [game] skip_intro (utakata docs/01-research.md, "Boot logos"): the opening scene (vtable 0x81441534,
// update 0x810040ae) steps through its states: 0-1 the Bandai Namco picture (3 s), 2-3 Spike's logo
// movie, 4-5 Acquire's, 6-7 the Havok picture (1.5 s), 8-9 the opening movie, 10 the title. A state
// is entered when the byte at +0xc is set, with +0x10 its timer. The application (*0x8146c2b0) keeps
// its scene manager at +0x1e4, and the manager the current scene at +4. While the first picture is
// up, the scene is moved on to the opening movie (Start skips that, as on the Vita): data only, so
// native code stays on.
// State 8's entry starts the movie player (*0x81512598: init, then play); its per-frame part only waits
// for the player's status (+0xc) to reach 2, the end. The write can race the game's own step from 0 to
// 1, whose entry ends by clearing +0xc: state 8 is then never entered, the player never starts, and the
// scene waits on a black screen forever (seen at 60 fps, where the scene runs twice as often). So the
// skip checks its result: in state 8 with the entry taken and the player still idle (status 0), it
// asks for the entry again.
constexpr Address OPENING_SCENE_VTABLE = 0x81441534;
constexpr Address MOVIE_PLAYER = 0x81512598;

struct IntroSkip {
    bool done = false;
    int writes = 0;
    int idle_ticks = 0; // ticks in state 8, entry taken, with the player idle
    int reentries = 0;
};

void skip_intro_logos(EmuEnvState &emuenv, IntroSkip &state) {
    if (state.done)
        return;
    const int64_t app = read_guest(emuenv, 0x8146c2b0, 4);
    const int64_t manager = app > 0 ? read_guest(emuenv, static_cast<Address>(app) + 0x1e4, 4) : -1;
    const int64_t scene = manager > 0 ? read_guest(emuenv, static_cast<Address>(manager) + 4, 4) : -1;
    if (scene <= 0 || read_guest(emuenv, static_cast<Address>(scene), 4) != OPENING_SCENE_VTABLE) {
        if (state.writes)
            state.done = true; // the opening scene has ended (the title)
        return;
    }
    const Address s = static_cast<Address>(scene);
    const int64_t current = read_guest(emuenv, s + 8, 4);
    if (current < 0)
        return;
    if (current >= 8 && !state.writes) {
        state.done = true;
        LOG_INFO("skip_intro: the logos had already played");
        return;
    }
    if (current == 8) {
        const int64_t player = read_guest(emuenv, MOVIE_PLAYER, 4);
        const int64_t status = player > 0 ? read_guest(emuenv, static_cast<Address>(player) + 0xc, 4) : -1;
        if (read_guest(emuenv, s + 0xc, 1) != 0 || status < 0)
            return; // the entry is still to come
        if (status != 0) {
            state.done = true;
            LOG_INFO("skip_intro: the boot logos skipped{}", state.reentries ? fmt::format(" (state 8 entered again {} times)", state.reentries) : "");
            return;
        }
        // the entry was lost, or the player didn't start: a quarter of a second's grace, then enter again
        if (++state.idle_ticks < 15)
            return;
        state.idle_ticks = 0;
        if (++state.reentries > 20) {
            state.done = true;
            LOG_WARN("skip_intro: the opening movie doesn't start");
            return;
        }
        const uint8_t enter = 1;
        std::memcpy(Ptr<uint8_t>(s + 0xc).get(emuenv.mem), &enter, 1);
        return;
    }
    if (current > 8) {
        state.done = true;
        LOG_INFO("skip_intro: the boot logos skipped");
        return;
    }
    if (current > 1) {
        // a logo movie is already playing: moving on now would start a second one on the same player
        state.done = true;
        LOG_INFO("skip_intro: too late, the logo movies have started");
        return;
    }
    // the opening movie, entered on the scene's next update (the game may move from 0 to 1 at the same
    // time; the next tick then writes again)
    const uint32_t opening = 8, zero = 0;
    const uint8_t enter = 1;
    std::memcpy(Ptr<uint8_t>(s + 0x10).get(emuenv.mem), &zero, 4);
    std::memcpy(Ptr<uint8_t>(s + 8).get(emuenv.mem), &opening, 4);
    std::memcpy(Ptr<uint8_t>(s + 0xc).get(emuenv.mem), &enter, 1);
    state.writes++;
}

// [cheats] (utakata docs/01-research.md, "Cheats"): values the game keeps in guest memory, set every
// tick while a cheat is on, after checking that they look as expected. Native code stays on.
// - The game's record (what a save holds, laid out as data0000.bin) is at 0x8153b530: money at
//   +0x1434; each character's stats from +0x1444, 0x124 apart (Zen, then Kaede): level, ?, SP, stamina,
//   offence, defence, ?, zankoku; ninja tools and items from +0x1bfb4 and the equipped ones from
//   +0x2a3b4, 0xc0 apart, each a 16-bit id and a 16-bit count.
// - In a mission, *0x814688c8 is the player (cleared when the mission ends; +0x18 is 1), and
//   *0x81469994 the characters (a count at +0x810, pointers 0x10 apart from +0x10). A character's
//   health is at +0xc of *(character + 0x8c), its maximum at +0x10.
constexpr Address RECORD = 0x8153b530;
constexpr Address PLAYER = 0x814688c8;
constexpr Address CHARACTERS = 0x81469994;
constexpr uint32_t ITEMS = 0x1bfb4, EQUIPPED = 0x2a3b4, ITEM_SIZE = 0xc0;
constexpr int ITEM_SLOTS = 304, EQUIPPED_SLOTS = 8;

struct CheatState {
    std::set<std::string> logged;
    // the item and count seen per item record, while Infinite ninja tools is on
    std::map<Address, std::pair<uint16_t, uint16_t>> counts;
};

uint32_t *guest_u32(EmuEnvState &emuenv, Address address) {
    if (!address || !is_valid_addr(emuenv.mem, address) || !is_valid_addr(emuenv.mem, address + 3))
        return nullptr;
    return Ptr<uint32_t>(address).get(emuenv.mem);
}

// a character's health record (current, maximum), when it looks like one
int32_t *health_of(EmuEnvState &emuenv, Address character) {
    const uint32_t *const record = guest_u32(emuenv, character + 0x8c);
    if (!record)
        return nullptr;
    int32_t *const health = reinterpret_cast<int32_t *>(guest_u32(emuenv, *record + 0xc));
    if (!health || !guest_u32(emuenv, *record + 0x10) || health[1] <= 0 || health[1] > 100000 || health[0] > health[1])
        return nullptr;
    return health;
}

void apply_cheats(EmuEnvState &emuenv, const game_frontend::Values &values, CheatState &state) {
    const auto log_once = [&](const std::string &what) {
        if (state.logged.insert(what).second)
            LOG_INFO("cheats: {}", what);
    };
    // the record: plausible values where the save has them
    uint32_t *const money = guest_u32(emuenv, RECORD + 0x1434);
    uint32_t *const level = guest_u32(emuenv, RECORD + 0x1444);
    const bool record = money && level && *money <= 99999999 && *level >= 1 && *level <= 99;
    if (record && values.get_bool("cheats.max_money") && *money != 9999999) {
        *money = 9999999;
        log_once("money 9,999,999");
    }
    if (record && values.get_bool("cheats.max_stats")) {
        // offsets from the character's level, and the values Apollo's patches write
        static constexpr std::pair<uint32_t, uint32_t> STATS[] = { { 0x0, 99 }, { 0x8, 255 }, { 0xc, 200 }, { 0x10, 100 }, { 0x14, 100 }, { 0x1c, 100 } };
        for (int c = 0; c < 2; c++)
            for (const auto &[offset, value] : STATS)
                if (uint32_t *const stat = guest_u32(emuenv, RECORD + 0x1444 + c * 0x124 + offset); stat && *stat < value)
                    *stat = value;
        log_once("stats raised");
    }
    // a mission: the player, and the characters
    const uint32_t *const player_ptr = guest_u32(emuenv, PLAYER);
    const Address player = player_ptr ? *player_ptr : 0;
    const uint32_t *const flag = player ? guest_u32(emuenv, player + 0x18) : nullptr;
    const bool mission = flag && *flag == 1;
    if (!mission) {
        state.counts.clear();
        return;
    }
    if (values.get_bool("cheats.infinite_health"))
        if (int32_t *const health = health_of(emuenv, player); health && health[0] < health[1]) {
            health[0] = health[1];
            log_once(fmt::format("health kept at {} ({:#x})", health[1], player));
        }
    if (values.get_bool("cheats.one_hit_kills")) {
        const uint32_t *const list_ptr = guest_u32(emuenv, CHARACTERS);
        const uint32_t *const count = list_ptr ? guest_u32(emuenv, *list_ptr + 0x810) : nullptr;
        if (count && *count <= 128) {
            int changed = 0;
            for (uint32_t i = 0; i < *count; i++) {
                const uint32_t *const character = guest_u32(emuenv, *list_ptr + 0x10 + i * 0x10);
                if (!character || !*character || *character == player)
                    continue;
                if (int32_t *const health = health_of(emuenv, *character); health && health[0] > 1) {
                    health[0] = 1;
                    changed++;
                }
            }
            if (changed)
                log_once("other characters' health kept at 1");
        }
    }
    if (values.get_bool("cheats.infinite_tools")) {
        // a count that drops is set back; one that grows (a pick-up) is the new count to keep
        const auto keep = [&](uint32_t table, int slots) {
            for (int i = 0; i < slots; i++) {
                const Address entry = RECORD + table + i * ITEM_SIZE;
                uint16_t *const item = reinterpret_cast<uint16_t *>(guest_u32(emuenv, entry));
                if (!item || item[0] == 0 || item[1] == 0 || item[1] > 999)
                    continue;
                // another item in this record (the list closed up): start from its count
                auto [seen, added] = state.counts.try_emplace(entry, item[0], item[1]);
                if (added || seen->second.first != item[0] || item[1] > seen->second.second)
                    seen->second = { item[0], item[1] };
                else if (item[1] < seen->second.second) {
                    item[1] = seen->second.second;
                    log_once("ninja tools and items given back");
                }
            }
        };
        keep(ITEMS, ITEM_SLOTS);
        keep(EQUIPPED, EQUIPPED_SLOTS);
    } else
        state.counts.clear();
}

// The camera's angles, for the mouse camera (utakata docs/01-research.md, "Mouse camera"): its world
// matrix is at +0xc0 of the application, row 0 the right vector (sin yaw, 0, -cos yaw), row 2 the
// forward vector, whose y is sin pitch. Both grow with the right stick pushed right and down.
std::optional<std::array<float, 2>> camera_angles(EmuEnvState &emuenv) {
    const int64_t app = read_guest(emuenv, 0x8146c2b0, 4);
    if (app <= 0)
        return std::nullopt;
    const Address m = static_cast<Address>(app) + 0xc0;
    if (!is_valid_addr(emuenv.mem, m) || !is_valid_addr(emuenv.mem, m + 47))
        return std::nullopt;
    float r[12];
    std::memcpy(r, Ptr<uint8_t>(m).get(emuenv.mem), sizeof(r));
    // a rotation: the right vector level and of unit length
    const float length = r[0] * r[0] + r[1] * r[1] + r[2] * r[2];
    if (!(std::abs(length - 1.0f) < 0.01f) || !(std::abs(r[1]) < 0.01f) || !(std::abs(r[9]) <= 1.0f))
        return std::nullopt;
    constexpr float degrees = 180.0f / 3.14159265f;
    return std::array<float, 2>{ std::atan2(r[0], -r[2]) * degrees, std::asin(r[9]) * degrees };
}

// The mouse camera's own way into the game (utakata D16, overrides/eboot.json, an eboot with the
// "mouse_camera" feature): the mouse's running totals in the page left over after the eboot's data,
// +0 'UTMC', +4 yaw (right) and +8 pitch (down) in 1/1024 of a degree, which each camera that turns by
// the right stick adds to its turn; +12 counts those cameras' updates. While it moves, the game takes
// the mouse; when it stops (no such camera now), the mouse camera falls back to the stick.
bool mouse_turn(EmuEnvState &emuenv, float yaw, float pitch, uint64_t now_ns) {
    constexpr Address BOX = 0x819f6f00;
    constexpr uint32_t MAGIC = 0x434d5455; // "UTMC"
    constexpr uint64_t QUIET_NS = 200'000'000;
    static bool has_feature = false;
    static uint32_t seen_count = 0;
    static uint64_t moved_ns = 0;
    static double totals[2] = {};
    if (!has_feature) {
        has_feature = aot_module_has_feature("eboot", "mouse_camera");
        if (!has_feature)
            return false;
        LOG_INFO("Mouse camera: the game's camera takes the mouse (eboot feature mouse_camera)");
    }
    if (!is_valid_addr(emuenv.mem, BOX) || !is_valid_addr(emuenv.mem, BOX + 15))
        return false;
    uint32_t *box = Ptr<uint32_t>(BOX).get(emuenv.mem);
    const uint32_t count = std::atomic_ref<uint32_t>(box[3]).load(std::memory_order_relaxed);
    if (count != seen_count) {
        seen_count = count;
        moved_ns = now_ns;
    }
    if (!moved_ns || now_ns - moved_ns > QUIET_NS)
        return false;
    if (box[0] != MAGIC) {
        // the totals start where the game's own copy is (a new game run starts from zero)
        totals[0] = static_cast<int32_t>(box[1]) / 1024.0;
        totals[1] = static_cast<int32_t>(box[2]) / 1024.0;
        std::atomic_ref<uint32_t>(box[0]).store(MAGIC, std::memory_order_release);
    }
    if (yaw != 0 || pitch != 0) {
        totals[0] += yaw;
        totals[1] += pitch;
        // 1/1024 degree steps, wrapping: the game takes the difference between two totals
        const auto steps = [](double degrees) { return static_cast<uint32_t>(static_cast<int64_t>(std::llround(std::fmod(degrees, 4194304.0) * 1024.0))); };
        std::atomic_ref<uint32_t>(box[1]).store(steps(totals[0]), std::memory_order_relaxed);
        std::atomic_ref<uint32_t>(box[2]).store(steps(totals[1]), std::memory_order_relaxed);
    }
    return true;
}

// [game] button_labels (utakata docs/01-research.md, "Button prompts"): the textures with PlayStation
// buttons in them, found as the game uploads them (UTAKATA_TEXTURE_DUMP below), and the symbols' boxes
// in them, upright: the game keeps its pictures upside down. Hashes and coordinates only.
std::vector<game_frontend::ButtonAtlas> button_atlases() {
    using Symbol = game_frontend::ButtonSymbol;
    return {
        // the UI font's page of button glyphs (prompts in text: "(X) OK (O) Back"), white symbols in
        // black discs; the sticks' L and R stay
        { 0x62b8e15ed100c438ull, 0x87000000u, 128, 512, "font page with the button glyphs",
            { { Symbol::Circle, 8, 8, 20, 21 }, { Symbol::Cross, 47, 11, 14, 14 }, { Symbol::Square, 9, 45, 18, 18 },
                { Symbol::Triangle, 44, 43, 20, 17 }, { Symbol::L, 54, 157, 9, 16 }, { Symbol::R, 45, 194, 11, 16 },
                { Symbol::Start, 16, 228, 40, 12 }, { Symbol::Select, 11, 264, 49, 12 } },
            true },
        // the HUD's atlas: a triangle in a ring, and circle, cross and square on discs of their own colour
        { 0x3a9fd8a58220c667ull, 0x87000000u, 512, 512, "HUD atlas",
            { { Symbol::Triangle, 225, 55, 31, 27 }, { Symbol::Circle, 350, 189, 30, 31, true },
                { Symbol::Cross, 406, 194, 22, 22, true }, { Symbol::Square, 456, 191, 28, 27, true } },
            true },
        // the pause menu's Basic Controls page, one picture per language: the red symbols on the right
        // (the cross's box as large as the others', so that its letter is as tall); the drawing of the
        // Vita stays as it is
        { 0x8b80ee57d17c6761ull, 0x85000000u, 1024, 1024, "Basic Controls (English)",
            { { Symbol::Square, 799, 74, 17, 17 }, { Symbol::Triangle, 798, 139, 21, 18 }, { Symbol::Circle, 799, 252, 20, 20 },
                { Symbol::Cross, 796, 368, 18, 18 } },
            true },
        { 0xde580ab9ec7ec20bull, 0x85000000u, 1024, 1024, "Basic Controls (French)",
            { { Symbol::Square, 878, 74, 16, 17 }, { Symbol::Triangle, 877, 139, 21, 18 }, { Symbol::Circle, 878, 252, 19, 20 },
                { Symbol::Cross, 879, 368, 18, 18 } },
            true },
        { 0xd5b790901de51996ull, 0x85000000u, 1024, 1024, "Basic Controls (Spanish)",
            { { Symbol::Square, 868, 74, 17, 17 }, { Symbol::Triangle, 868, 139, 20, 18 }, { Symbol::Circle, 868, 252, 20, 20 },
                { Symbol::Cross, 870, 368, 18, 18 } },
            true },
    };
}

// Development (UTAKATA_TEXTURE_DUMP=<folder>): every texture the game uploads in the formats its UI
// uses, written once as a PNG named <format>-<width>x<height>-<FNV-1a 64 of its first level, as
// game_frontend::ButtonLabels hashes it>.png, decoded as swizzled. Nothing is replaced. For finding
// the textures with PlayStation buttons in them.
class TextureDump : public renderer::TextureOverride {
public:
    explicit TextureDump(fs::path dir)
        : dir(std::move(dir)) {
        fs::create_directories(this->dir);
    }
    bool wants(SceGxmTextureBaseFormat format, uint32_t width, uint32_t height) const override {
        return bytes_of(format, width, height) && width >= 8 && height >= 8 && width <= 2048 && height <= 2048;
    }
    bool make(SceGxmTextureBaseFormat format, uint32_t width, uint32_t height, const uint8_t *data, uint32_t size,
        std::vector<uint8_t> &, uint32_t &, uint32_t &) override {
        const uint32_t bytes = bytes_of(format, width, height);
        if (!bytes || size < bytes)
            return false;
        uint64_t hash = 0xcbf29ce484222325ull;
        for (uint32_t i = 0; i < bytes; i++)
            hash = (hash ^ data[i]) * 0x100000001b3ull;
        {
            std::lock_guard lock(mutex);
            if (!done.insert(hash).second)
                return false;
        }
        std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4, 255);
        switch (format) {
        case SCE_GXM_TEXTURE_BASE_FORMAT_U8: {
            std::vector<uint8_t> linear(bytes);
            renderer::texture::swizzled_texture_to_linear_texture(linear.data(), data, width, height, 8);
            for (size_t i = 0; i < linear.size(); i++)
                rgba[i * 4] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = linear[i];
            break;
        }
        case SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8:
            renderer::texture::swizzled_texture_to_linear_texture(rgba.data(), data, width, height, 32);
            break;
        default: {
            std::vector<uint8_t> blocks(bytes);
            renderer::texture::resolve_z_order_compressed_texture(format, blocks.data(), data, width, height);
            renderer::texture::decompress_compressed_texture(format, rgba.data(), blocks.data(), width, height);
        }
        }
        const fs::path file = dir / fmt::format("{:x}-{}x{}-{:016x}.png", fmt::underlying(format), width, height, hash);
        stbi_write_png(fs_utils::path_to_utf8(file).c_str(), width, height, 4, rgba.data(), width * 4);
        return false;
    }

private:
    static uint32_t bytes_of(SceGxmTextureBaseFormat format, uint32_t width, uint32_t height) {
        switch (format) {
        case SCE_GXM_TEXTURE_BASE_FORMAT_UBC1: return width * height / 2;
        case SCE_GXM_TEXTURE_BASE_FORMAT_UBC2:
        case SCE_GXM_TEXTURE_BASE_FORMAT_UBC3: return width * height;
        case SCE_GXM_TEXTURE_BASE_FORMAT_U8: return width * height;
        case SCE_GXM_TEXTURE_BASE_FORMAT_U8U8U8U8: return width * height * 4;
        default: return 0;
        }
    }
    fs::path dir;
    std::mutex mutex;
    std::set<uint64_t> done;
};

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
    profile.default_touches = { { "caution", 896, 260, "F", "righttrigger", "Caution (touch)" } };
    profile.touch_targets["caution"] = caution_eye;
    // crouching (sneaking, clinging to a wall) is R held: on a phone's on-screen controls, a toggle holds
    // it while the thumbs move and attack (utakata Phase 11d)
    profile.screen_latch = { SCE_CTRL_R1, "CROUCH" };
    // the right stick turns the camera at one speed (utakata docs/01-research.md, "Mouse camera"): past
    // 77 steps of 127 from the centre, 120 degrees a second left and right and 60 up and down; nothing
    // closer to the centre
    profile.stick_response.points = { { 76.0f / 127.0f, 0.0f }, { 77.0f / 127.0f, 120.0f }, { 1.0f, 120.0f } };
    profile.stick_response_y = game_frontend::StickResponse{ { { 76.0f / 127.0f, 0.0f }, { 77.0f / 127.0f, 60.0f }, { 1.0f, 60.0f } } };
    // it also speeds up while the stick is held (0.27 degrees a frame more each frame, at 30 fps), so the
    // mouse camera counts what it really turned
    profile.camera_angles = camera_angles;
    // with the eboot's overrides the camera takes the mouse's motion itself, with no speed limit
    profile.mouse_turn = mouse_turn;
    profile.usage = "Saves (then quit):\n"
                    "  --import-save <folder|zip>  copy decrypted Vita saves (e.g. from a Vita) in, keeping the\n"
                    "                              old ones in shinobido2/save-backups/\n"
                    "  --export-save <folder>      copy saves out\n"
                    "  --slot <0-9>                one save slot for the two above (default: every slot)\n"
                    "  --quiet                     no message box for the two above (the result is in the log)\n";
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
    profile.hooks.configure = [](game_frontend::Context &ctx, Config &cfg) {
        // COLOR0/COLOR1 clamped to [0, 1]: without it the first mission's walls are near black and the
        // scene is tinted blue; with it the colours match the Vita's (as ss-delta's fog)
        cfg.clamp_color_varyings = true;
        // [game] language: the system language the game asks SceAppUtil for at boot
        cfg.sys_lang = LANGUAGES.at(ctx.settings->values.get("game.language"));
    };
    profile.hooks.before_run = [](game_frontend::Context &ctx) {
        const bool effects = ctx.settings->values.get_bool("audio.effects");
        ctx.emuenv->ngs.effects.reverb = effects;
        ctx.emuenv->ngs.effects.release = effects;
        LOG_INFO("audio effects (reverb, fade-outs): {}", effects ? "on" : "off");
    };
    FrameRate frame_rate;
    IntroSkip intro_skip;
    CheatState cheats;
    std::unique_ptr<TextureDump> texture_dump;
    if (const char *dir = std::getenv("UTAKATA_TEXTURE_DUMP"))
        texture_dump = std::make_unique<TextureDump>(fs_utils::utf8_to_path(dir));
    // [game] button_labels: the textures with PlayStation buttons redrawn with Xbox labels as they are
    // uploaded; the settings menu switches it
    game_frontend::ButtonLabels button_labels(button_atlases());
    profile.hooks.on_tick = [&](game_frontend::Context &ctx) {
        if (renderer::TextureCache *cache = ctx.emuenv->renderer ? ctx.emuenv->renderer->get_texture_cache() : nullptr) {
            if (texture_dump) {
                if (cache->texture_override.load() != texture_dump.get()) {
                    cache->texture_override = texture_dump.get();
                    cache->refresh_texture_override();
                }
            } else {
                // auto: the device used last, and the first connected gamepad's own labels (both may change
            // while the game runs)
                std::string style = ctx.settings->values.get("game.button_labels");
                std::map<std::string, std::string> face;
                if (style == "auto")
                    style = game_frontend::auto_button_style(ctx.emuenv->ctrl, face, ctx.last_input);
                const bool changed = button_labels.configure(style, ctx.settings->gamepad, static_cast<int>(std::lround(ctx.settings->scale)), face,
                    ctx.settings->keyboard);
                if (cache->texture_override.load() != &button_labels) {
                    cache->texture_override = &button_labels;
                    cache->refresh_texture_override();
                } else if (changed)
                    cache->refresh_texture_override();
            }
        }
        apply_cheats(*ctx.emuenv, ctx.settings->values, cheats);
        if (ctx.settings->values.get_int("display.frame_rate") == 60)
            update_frame_rate(*ctx.emuenv, frame_rate);
        if (ctx.settings->values.get_bool("game.skip_intro"))
            skip_intro_logos(*ctx.emuenv, intro_skip);
    };
    return game_frontend::run(profile, argc, argv);
}
