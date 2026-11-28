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

// re-nova.exe: Phantasy Star Nova (PCSG00351, update 1.05) on the shared single-game frontend
// (game-frontend/profile.h). The re-nova project's docs describe the port.
//
#include "../../game-frontend/profile.h"

#include <config/state.h>
#include <cpu/aot.h>
#include <emuenv/state.h>
#include <mem/ptr.h>
#include <packages/sfo.h>
#include <util/log.h>

#include <cstdlib>

// provides WinMain for the windowed executable, which calls main below
#include <SDL3/SDL_main.h>

namespace {

namespace layout = game_frontend::layout;

constexpr const char *TITLE_ID = "PCSG00351";
constexpr const char *PATCH_CPK = "NOVA_FileList_Vita_patch.cpk";

constexpr Address FRAME_CLOCK_PTR = 0x82474314;
constexpr Address FRAME_RATE_OFFSET = 0xcc;
constexpr Address PHYSICS_STEP = 0x82474334;
constexpr float VITA_PHYSICS_STEP = 1.0f / 31.0f;
constexpr float PHYSICS_STEP_60 = 0.016f;

bool apply_frame_rate(EmuEnvState &emuenv, int current, int requested) {
    auto &mem = emuenv.mem;
    const Ptr<Address> clock_ptr(FRAME_CLOCK_PTR);
    const Ptr<float> step_ptr(PHYSICS_STEP);
    if (emuenv.app_info.app_version != "1.05" || !clock_ptr.valid(mem) || !step_ptr.valid(mem))
        return false;

    const Address clock = *clock_ptr.get(mem);
    const Ptr<uint32_t> rate_ptr(clock + FRAME_RATE_OFFSET);
    if (!clock || clock % alignof(uint32_t) || rate_ptr.address() < clock || !rate_ptr.valid(mem))
        return false;

    uint32_t &rate = *rate_ptr.get(mem);
    float &step = *step_ptr.get(mem);
    // Startup sets both values in 1.05. Wait for that initialization and for the pair we expect.
    if (rate != static_cast<uint32_t>(current) || step != (current == 30 ? VITA_PHYSICS_STEP : PHYSICS_STEP_60))
        return false;

    if (requested == 60 && !aot_module_has_feature("eboot", "camera_vblank_step")) {
        static const bool warned = [] {
            LOG_WARN("frame_rate: 60 FPS requires an eboot module built with the camera timing override");
            return true;
        }();
        (void)warned;
        return false;
    }

    // The limiter waits 60 / rate vblanks. Physics uses a separate fixed step.
    step = requested == 30 ? VITA_PHYSICS_STEP : PHYSICS_STEP_60;
    rate = requested;
    LOG_INFO("frame_rate: {} FPS (physics step {})", requested, step);
    return true;
}

// The update's archive, which the English patch replaces (whole: it holds 1.05's files too), by size.
// Sizes are enough to tell the builds apart and cost nothing at start; the project's hashes.md has
// their SHA-1s.
const char *patch_build(uintmax_t size) {
    switch (size) {
    case 154735600: return "none (the update's own archive: the game is in Japanese)";
    case 233072176: return "English patch 1.0 (\"v1.0.1\", 2026-11), the reference build";
    case 232657896: return "English patch of 2024-08 (older than 1.0: it can crash, update it)";
    default: return nullptr;
    }
}

void log_patch(const EmuEnvState &emuenv, bool repatch) {
    const fs::path ux0 = emuenv.vita_fs_path / "ux0";
    // a rePatch mod is read in place of the installed file while [game] repatch is on
    const fs::path mod = ux0 / "rePatch" / TITLE_ID / PATCH_CPK;
    const fs::path installed = ux0 / "app" / TITLE_ID / PATCH_CPK;
    boost::system::error_code ec;
    const fs::path &file = repatch && fs::exists(mod, ec) ? mod : installed;
    const uintmax_t size = fs::file_size(file, ec);
    if (ec) {
        LOG_WARN("English patch: no {} found ({})", PATCH_CPK, file.string());
        return;
    }
    const char *build = patch_build(size);
    LOG_INFO("English patch: {} ({}, {} bytes)", build ? build : "an unknown build (a newer release or a beta?)",
        &file == &mod ? "read from rePatch" : "the installed file", size);
}

game_frontend::IniLayout ini_layout() {
    using Kind = game_frontend::SettingSpec::Kind;
    const std::string id = "re-nova", exe = id + game_frontend::executable_suffix;
    game_frontend::SettingSpec frame_rate{ "frame_rate", Kind::Choice, "30", 0, 0, { "30", "60" },
        "30 FPS as on the Vita, or experimental 60 FPS on update 1.05." };
    frame_rate.title = "Frame rate";
    frame_rate.live = true;
    game_frontend::SettingSpec mapping{ "memory_mapping", Kind::Choice, "double-buffer", 0, 0,
        { "double-buffer", "external-host", "page-table", "native-buffer" },
        "How the game's memory is shared with the graphics card (Vita3K's memory mapping). Read\n"
        "when the game starts." };
    mapping.title = "Memory mapping";
    return {
        "Re:NOVA settings. Lines starting with ; are comments; delete the ; to use a line.",
        {
            layout::paths(TITLE_ID, id, exe),
            { "display", "",
                { layout::scale(), frame_rate, layout::window_scale(), layout::fullscreen(id, exe), layout::vsync(),
                    layout::filter(), layout::fps_counter(), layout::mipmaps(true), mapping } },
            layout::audio(),
            { "controls", "",
                { { "pstv_mode", Kind::Bool, "false", 0, 0, {},
                    "Run as a PS TV, where the game takes a DualShock's L2/R2 and L3/R3 and has no\n"
                    "touchscreens. Being studied. Read when the game starts." } } },
            { "game", "", { layout::repatch() } },
            layout::recompiled(id),
            layout::debug(id),
            layout::keyboard(),
            layout::gamepad(),
            layout::touch("; None yet.\n"),
        },
    };
}

} // namespace

int main(int argc, char *argv[]) {
    game_frontend::Profile profile;
    profile.title_id = TITLE_ID;
    profile.name = "Re:NOVA";
    profile.id = "re-nova";
    profile.smooth_mip_sampling = true;
    profile.ini = ini_layout();
    profile.hooks.configure = [](game_frontend::Context &ctx, Config &cfg) {
        // a PS TV: pads on the external ports, the PS TV model (SceCtrl, SceSysmem)
        cfg.pstv_mode = ctx.settings->values.get_bool("controls.pstv_mode");
        cfg.memory_mapping = ctx.settings->values.get("display.memory_mapping");
        // RENOVA_MEMORY_MAPPING (debug): overrides the setting for a run
        if (const char *mapping = std::getenv("RENOVA_MEMORY_MAPPING"); mapping && *mapping)
            cfg.memory_mapping = mapping;
        LOG_INFO("memory mapping: {}", cfg.memory_mapping);
        // Nova's environment textures ship with truncated mip chains (1-5 levels) and -1..-2 LOD
        // bias, causing heavy ground shimmer in motion. When [display] mipmaps is on, pair full
        // mip chains and clamped LOD bias with 16x anisotropic filtering so oblique ground stays
        // sharp without aliasing.
        if (ctx.settings->values.get_bool("display.mipmaps"))
            cfg.anisotropic_filtering = 16;
        // Nova's quarter-resolution post-process uses USSE's base-2 exponentials.
        cfg.exp_log_base2 = true;
        // a draw whose pipeline is still compiling waits for it (up to 250 ms, from a budget that refills
        // at a quarter of the time passing) rather than being skipped: cutscenes, mostly seen once,
        // showed characters missing until a camera cut. With an empty cache the opening waited 120 ms in
        // all (54 ms at most in a frame) and drew everything a warm cache draws
        cfg.pipeline_wait_ms = 250;
    };
    profile.hooks.before_mods = [](game_frontend::Context &ctx, bool repatch) {
        log_patch(*ctx.emuenv, repatch);
    };
    profile.hooks.on_tick = [current = 30](game_frontend::Context &ctx) mutable {
        const int requested = ctx.settings->values.get("display.frame_rate") == "60" ? 60 : 30;
        if (requested != current && apply_frame_rate(*ctx.emuenv, current, requested))
            current = requested;
    };
    return game_frontend::run(profile, argc, argv);
}
