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
    float scale = 1.0f; // internal resolution multiplier
    int window_scale = 1; // window size as a multiple of 960x544
    bool fullscreen = false;
    bool vsync = true;
    std::string filter = "Bilinear";
    // [audio]
    int volume = 100;
    // [recompiled]
    bool use_recompiled = true;
    // [debug]
    int log_level = 2; // spdlog: 0 trace ... 6 off
    bool validation_layer = false;

    // [keyboard] and [gamepad]: Vita button -> keys / gamepad button, applied by input.cpp
    std::map<std::string, std::string> keyboard, gamepad;

    std::vector<std::string> warnings; // lines that could not be used
};

// Reads ss-delta.ini from `dir`, writing the commented default first when there is none.
Settings load_settings(const fs::path &dir);

} // namespace ss_delta
