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
// (game-frontend/profile.h). The shinobido-2 project's docs describe the port.

#include "../../game-frontend/profile.h"

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
            layout::touch(""),
        },
    };
}

} // namespace

int main(int argc, char *argv[]) {
    game_frontend::Profile profile;
    profile.title_id = TITLE_ID;
    profile.name = "Shinobido 2: Revenge of Zen";
    profile.id = "shinobido2";
    profile.ini = ini_layout();
    return game_frontend::run(profile, argc, argv);
}
