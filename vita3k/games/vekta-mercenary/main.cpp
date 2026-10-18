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

// vekta-mercenary.exe: Killzone Mercenary (EU, PCSF00403, update 1.12) on the shared single-game
// frontend (game-frontend/profile.h). The vekta-mercenary project's docs describe the port.
//
// Phase 1 of that project: the game's profile and settings file only. Its saves, controls (the
// game's own PS TV mode) and resolution come in later phases.

#include "../../game-frontend/profile.h"

#include <config/state.h>
#include <emuenv/state.h>
#include <ngs/state.h>
#include <util/log.h>

// provides WinMain for the windowed executable, which calls main below
#include <SDL3/SDL_main.h>

namespace {

namespace layout = game_frontend::layout;

constexpr const char *TITLE_ID = "PCSF00403";

// [audio]: the shared volume, and the NGS modules Vita3K left out (ngs::State::Effects)
game_frontend::IniSection audio_layout() {
    using Kind = game_frontend::SettingSpec::Kind;
    game_frontend::IniSection section = layout::audio();
    section.settings.push_back({ "effects", Kind::Bool, "true", 0, 0, {},
        "The game's filters, equalizer, compressor and distortion (muffled sound through walls, the\n"
        "mix's dynamics; false: the unprocessed sound of earlier builds, for comparison). Read when the\n"
        "game starts." });
    section.settings.back().title = "Sound effects (filters, compressor)";
    return section;
}

game_frontend::IniLayout ini_layout() {
    const std::string id = "vekta-mercenary", exe = "vekta-mercenary.exe";
    return {
        "vekta-mercenary settings. Lines starting with ; are comments; delete the ; to use a line.",
        {
            layout::paths(TITLE_ID, id, exe),
            { "display", "",
                { layout::scale(), layout::window_scale(), layout::fullscreen(id, exe), layout::vsync(),
                    layout::filter(), layout::mipmaps(false) } },
            audio_layout(),
            { "controls", "",
                { { "pstv_mode", game_frontend::SettingSpec::Kind::Bool, "false", 0, 0, {},
                    "Run as a PS TV, where the game takes a DualShock and has no touchscreens (its\n"
                    "update 1.11 controls). Being studied (the project's D8). Read when the game starts." } } },
            layout::recompiled(id),
            layout::debug(id),
            layout::keyboard(),
            layout::gamepad(),
            layout::touch("; None yet: the game's touch actions are to come from its own PS TV controls.\n"),
        },
    };
}

} // namespace

int main(int argc, char *argv[]) {
    game_frontend::Profile profile;
    profile.title_id = TITLE_ID;
    profile.name = "Killzone Mercenary";
    profile.id = "vekta-mercenary";
    profile.ini = ini_layout();
    profile.hooks.configure = [](game_frontend::Context &ctx, Config &cfg) {
        // the game's shaders hang the GPU at its first level without this
        // (shader/src/translator/data.cpp, vpck)
        cfg.vpck_zero_fill = true;
        // exponents and logarithms in base 2, as the hardware computes them (shader/src/translator/alu.cpp)
        cfg.exp_log_base2 = true;
        // the box shader of its occlusion tests reads a register before writing it; AMD's compiler let
        // the undefined value spread into the box address and every AI character was culled
        cfg.zero_init_registers = true;
        // its particle scene loads the quarter-resolution depth, draws into it and doesn't store it; on the
        // hardware the depth of field after it reads the depth stored before, not those writes (blur sweeps)
        cfg.depth_store_scenes = true;
        // a PS TV: pads on the external ports, the PS TV model (SceCtrl, SceSysmem)
        cfg.pstv_mode = ctx.settings->values.get_bool("controls.pstv_mode");
    };
    profile.hooks.before_run = [](game_frontend::Context &ctx) {
        const bool effects = ctx.settings->values.get_bool("audio.effects");
        ctx.emuenv->ngs.effects.dsp = effects;
        LOG_INFO("audio effects (filters, equalizer, compressor, distortion): {}", effects ? "on" : "off");
    };
    return game_frontend::run(profile, argc, argv);
}
