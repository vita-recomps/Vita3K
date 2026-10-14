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

#include <mem/ptr.h>

#include <vector>

struct MemState;

namespace ngs {
struct VoiceDefinition;
struct System;

struct State {
    Ptr<VoiceDefinition> definitions;
    std::vector<System *> systems;
    // effects Vita3K long left out, which change a game's sound: on per game (its single-game front
    // end), off by default, so other games sound as before
    struct Effects {
        bool reverb = false; // the reverb buss and its presets (sceNgsModuleGetPreset)
        bool release = false; // sceNgsVoiceKeyOff fades a voice out over its envelope's release time
    } effects;
};

bool init(State &ngs, MemState &mem);
void deinit(State &ngs, MemState &mem);
} // namespace ngs
