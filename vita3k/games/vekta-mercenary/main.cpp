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

// provides WinMain for the windowed executable, which calls main below
#include <SDL3/SDL_main.h>

namespace {

namespace layout = game_frontend::layout;

constexpr const char *TITLE_ID = "PCSF00403";

game_frontend::IniLayout ini_layout() {
    const std::string id = "vekta-mercenary", exe = "vekta-mercenary.exe";
    return {
        "vekta-mercenary settings. Lines starting with ; are comments; delete the ; to use a line.",
        {
            layout::paths(TITLE_ID, id, exe),
            { "display", "",
                { layout::scale(), layout::window_scale(), layout::fullscreen(id, exe), layout::vsync(),
                    layout::filter(), layout::mipmaps(false) } },
            layout::audio(),
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
    profile.hooks.configure = [](game_frontend::Context &, Config &cfg) {
        // the game's shaders hang the GPU at its first level without this
        // (shader/src/translator/data.cpp, vpck)
        cfg.vpck_zero_fill = true;
    };
    return game_frontend::run(profile, argc, argv);
}
