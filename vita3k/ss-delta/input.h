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

// Keyboard and gamepad bindings from ss-delta.ini, and keyboard input for the SDL frontend (which,
// unlike the Qt interface, does not turn keys into Vita buttons itself).
#pragma once

#include <input/physical_key.h>

#include <map>
#include <set>
#include <string>
#include <vector>

struct Config;
struct EmuEnvState;

namespace ss_delta {

// Applies [keyboard] and [gamepad] (Vita button -> keys / gamepad button) to the config; lines
// that can't be used are added to `warnings`.
void apply_bindings(Config &cfg, const std::map<std::string, std::string> &keyboard,
    const std::map<std::string, std::string> &gamepad, std::vector<std::string> &warnings);

// Keys held down, turned into buttons and stick axes in the controller's keyboard state
class Keyboard {
public:
    // an SDL scancode went down or up
    void key(EmuEnvState &emuenv, int scancode, bool pressed);
    // the window lost focus: release everything
    void release_all(EmuEnvState &emuenv);

private:
    void update(EmuEnvState &emuenv);
    std::set<input::PhysicalKeyCode> pressed;
};

} // namespace ss_delta
