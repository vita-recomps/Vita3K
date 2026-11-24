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

#include "input.h"
#include "settings.h"

#include <util/fs.h>

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct Config;
struct EmuEnvState;

namespace game_frontend {

// a game's executable is <id> followed by this (CMake's add_game): <id>.exe, <id>.arm64 on macOS, or
// <id>.x86_64 on Linux
#ifdef _WIN32
inline constexpr const char *executable_suffix = ".exe";
#elif defined(__APPLE__)
inline constexpr const char *executable_suffix = ".arm64";
#else
inline constexpr const char *executable_suffix = ".x86_64";
#endif

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
    // once the game is known to be installed, before the rePatch mods are read: a game may install or
    // remove packaged mods of its own here (packaged_mods.h); `repatch` is [game] repatch
    std::function<void(Context &, bool repatch)> before_mods;
    // once the game is known to be installed, before it starts (add-ons, the game's files, GXM)
    std::function<void(Context &)> before_run;
    // every frame of the frontend's loop
    std::function<void(Context &)> on_tick;
    // --dump-elfs: right before the dump run quits (load modules the game loads only later)
    std::function<void(Context &)> before_dump_quit;
};

struct Profile {
    std::string title_id; // the installed app: ux0/app/<title_id>
    // the same game's title IDs in other regions: when title_id is not installed, the first of these
    // that is runs instead (--title-id <id> picks one), and title_id is set to it before any hook runs
    std::vector<std::string> other_title_ids;
    std::string name; // the game's name: window title, messages
    std::string id; // file names: <id>.ini, <id>/ (config, log), <id>_<date>.png screenshots
    // the settings file: its layout, from which the default file is written and the file is read
    IniLayout ini;
    // [touch] bindings when the file has no [touch] section at all (a file from before it existed)
    std::vector<TouchBinding> default_touches;
    // [touch] bindings whose point the game decides (a HUD element that moves), by binding name
    std::map<std::string, Touches::Target> touch_targets;
    // Android's on-screen controls: a small button that holds a Vita button (SceCtrlButtons, as the
    // extended functions read it: SCE_CTRL_L1) down from one tap to the next, for an action the game
    // wants held (ss-delta: L, the lock-on). None when `label` is empty.
    struct ScreenLatch {
        uint32_t button = 0;
        std::string label;
    } screen_latch;
    // how the game's right stick turns its camera, for the mouse camera ([mouse], layout::mouse): left
    // and right (the yaw), and up and down (the pitch) when it differs; empty: as left and right
    StickResponse stick_response;
    std::optional<StickResponse> stick_response_y;
    // the camera's angles, for a game whose camera speeds up while the stick is held: the mouse camera
    // then counts what the camera really turned (MouseCamera::CameraAngles); empty: from the table
    MouseCamera::CameraAngles camera_angles;
    // a game whose own code takes the mouse's motion (MouseCamera::DirectTurn); empty: the stick only
    MouseCamera::DirectTurn mouse_turn;
    // Ad Hoc Play: --player 2-4 run more copies with their own Vita user, config and log
    bool multiplayer = false;
    // the executable has an icon resource with id 101 (its .rc); the window then uses it
    bool icon_resource = false;
    // the game's own command-line options (hooks.parse_arg), for --help: lines as in its usage list
    std::string usage;
    Hooks hooks;
};

int run(Profile &profile, int argc, char *argv[]);

} // namespace game_frontend
