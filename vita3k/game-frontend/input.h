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

#include "settings.h"

#include <input/physical_key.h>

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

struct Config;
struct EmuEnvState;

namespace game_frontend {

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
    bool down(input::PhysicalKeyCode code) const { return pressed.contains(code); }

private:
    void update(EmuEnvState &emuenv);
    std::set<input::PhysicalKeyCode> pressed;
};

// [touch]: fingers on the front touchscreen while their keys or gamepad button are held, and the
// ones the test driver holds through the input pipe
class Touches {
public:
    // a touch whose point moves (a game's HUD element): the point each frame in 960x544 screen
    // pixels, or none (no finger goes down while there is nothing to touch)
    using Target = std::function<std::optional<std::pair<float, float>>(EmuEnvState &)>;
    void configure(const std::vector<TouchBinding> &touches, std::vector<std::string> &warnings,
        const std::map<std::string, Target> &targets = {});
    // once a frame: the held fingers into the touch state
    void update(EmuEnvState &emuenv, const Keyboard &keyboard);

private:
    struct Binding {
        float x = 0, y = 0;
        std::vector<input::PhysicalKeyCode> keys;
        int gamepad_button = -1; // SDL_GamepadButton
        int gamepad_axis = -1; // SDL_GamepadAxis (a trigger)
        Target target; // when set, it gives the point instead of x, y
    };
    std::vector<Binding> bindings;
};

} // namespace game_frontend
