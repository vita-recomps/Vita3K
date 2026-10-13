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

// A single-game executable on the SDL frontend, without the Qt interface: the shared frontend
// (frontend.cpp) runs one installed game, described by a Profile. Each game's executable
// (games/<id>/main.cpp) fills in its Profile and calls game_frontend::run.

#include "settings.h"

#include <util/fs.h>

#include <functional>
#include <optional>
#include <string>
#include <vector>

struct Config;
struct EmuEnvState;

namespace game_frontend {

// What the hooks see. Fields that exist only once the emulator runs are null before that.
struct Context {
    EmuEnvState *emuenv = nullptr;
    const Settings *settings = nullptr;
    fs::path exe_dir; // the executable's folder
    fs::path data_dir; // <id>/ next to the executable (player N: <id>/player<N>/): config, log
    int player = 1; // --player N
    std::string user_id; // the Vita user of this player ("00" for player 1)
    std::string window_name;
    std::vector<std::string> args; // the command line, without the executable
};

struct Hooks {
    // extra command-line options; returns true when it took args[i] (and moves i past any value)
    std::function<bool(Context &, const std::vector<std::string> &args, size_t &i)> parse_arg;
    // after the settings and the log are ready, before the emulator starts; a value ends the
    // program with that exit code (one-shot commands such as a save import)
    std::function<std::optional<int>(Context &)> before_init;
    // the emulator's config, after the frontend's own defaults
    std::function<void(Context &, Config &)> configure;
    // once the game is known to be installed, before it starts (add-ons, the game's files, GXM)
    std::function<void(Context &)> before_run;
    // every frame of the frontend's loop
    std::function<void(Context &)> on_tick;
    // --dump-elfs: right before the dump run quits (load modules the game loads only later)
    std::function<void(Context &)> before_dump_quit;
};

struct Profile {
    std::string title_id; // the installed app: ux0/app/<title_id>
    std::string name; // the game's name: window title, messages
    std::string id; // file names: <id>.ini, <id>/ (config, log), <id>_<date>.png screenshots
    // the settings file: its layout, from which the default file is written and the file is read
    IniLayout ini;
    // [touch] bindings when the file has no [touch] section at all (a file from before it existed)
    std::vector<TouchBinding> default_touches;
    // Ad Hoc Play: --player 2-4 run more copies with their own Vita user, config and log
    bool multiplayer = false;
    // the executable has an icon resource with id 101 (its .rc); the window then uses it
    bool icon_resource = false;
    Hooks hooks;
};

int run(Profile &profile, int argc, char *argv[]);

} // namespace game_frontend
