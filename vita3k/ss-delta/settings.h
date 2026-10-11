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

// ss-delta.ini: the player-facing settings of ss-delta.exe. Everything else is fixed.
#pragma once

#include <util/fs.h>

#include <map>
#include <string>
#include <vector>

namespace ss_delta {

struct Settings {
    // [paths]
    fs::path vita_fs; // the Vita file system holding the installed game (ux0/app/PCSA00152)
    fs::path recompiled; // where ssd_aot_<module>.dll are
    // [display]
    bool full_3d = true; // 3D at 960x544 (boot_config.ini ResoDown 0) instead of 720x408
    float scale = 1.0f; // internal resolution multiplier
    int window_scale = 1; // window size as a multiple of 960x544
    bool fullscreen = false;
    bool vsync = true;
    std::string filter = "Bilinear";
    // [audio]
    int volume = 100;
    // [game]
    bool dlc = true; // record the add-ons (whose content is in the game's data) in the save
    // [recompiled]
    bool use_recompiled = true;
    bool jit = false; // keep the JIT for guest code without native code (NativeCPU otherwise)
    // [debug]
    int log_level = 2; // spdlog: 0 trace ... 6 off
    bool validation_layer = false;
    int frame_rate = 30; // the game's frame timer and display flip rate (study: 30 or 60)
    int frame_rate_delay = 0; // seconds after start before frame_rate applies (study)

    // [keyboard] and [gamepad]: Vita button -> keys / gamepad button, applied by input.cpp
    std::map<std::string, std::string> keyboard, gamepad;

    // [touch]: a finger held on the front touchscreen at a point while its keys or gamepad
    // button are held (input.cpp)
    struct TouchBinding {
        std::string name;
        float x = 0, y = 0; // 960x544 screen pixels
        std::string keys; // up to two, as in [keyboard]
        std::string gamepad; // one button as in [gamepad], or lefttrigger / righttrigger
    };
    std::vector<TouchBinding> touches;

    // [boot_config]: keys of the game's boot_config.ini to override, case kept
    std::map<std::string, std::string> boot_config;

    std::vector<std::string> warnings; // lines that could not be used
};

// Reads ss-delta.ini from `dir`, writing the commented default first when there is none.
Settings load_settings(const fs::path &dir);

} // namespace ss_delta
