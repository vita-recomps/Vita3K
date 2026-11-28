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

#include <ctrl/ctrl.h>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_haptic.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

typedef std::shared_ptr<SDL_Gamepad> GamepadPtr;
typedef std::shared_ptr<SDL_Haptic> HapticPtr;

struct Controller {
    GamepadPtr controller;
    int port; // SDL_Gamepad index
    bool has_accel;
    bool has_gyro;
    bool has_led;
    const char *name;
};

struct ControllerBinding {
    SDL_GamepadButton controller;
    uint32_t button;
};

struct SDL_GUIDComparator {
    bool operator()(const SDL_GUID &a, const SDL_GUID &b) const {
        return memcmp(&a, &b, sizeof(a)) < 0;
    }
};

typedef std::map<SDL_GUID, Controller, SDL_GUIDComparator> ControllerList;

// A gamepad input beyond Vita3K's one button per Vita button (Config::controller_binds): a front end's
// second button for a Vita button, or a trigger (which presses L2/R2 as well)
struct ExtraGamepadBinding {
    int button = -1; // an SDL_GamepadButton, or -1
    int axis = -1; // an SDL_GamepadAxis (a trigger), or -1
    uint32_t vita_button = 0; // the Vita button in the basic functions
    uint32_t vita_button_ext = 0; // the same in the extended ones (L1/R1 for L/R)
};

struct VirtualKeyboardState {
    uint32_t buttons = 0;
    uint32_t buttons_ext = 0;
    float axes[4] = {};
};

struct CtrlState {
    std::mutex mutex;
    ControllerList controllers;
    int controllers_num = 0;
    // ss-delta: n > 0 opens only the nth connected gamepad (one copy of the game per player)
    int only_gamepad = 0;
    float analog_multiplier = 1.0f;
    // ss-delta: gamepad inputs a front end adds to controller_binds (under `mutex`)
    std::vector<ExtraGamepadBinding> extra_gamepad_binds;
    // Physical buttons claimed by the single-game frontend, which take priority over implicit PS/L3/R3.
    std::vector<short> claimed_gamepad_buttons;
    bool has_motion_support = false;
    bool free_ports[SCE_CTRL_MAX_WIRELESS_NUM] = { true, true, true, true };
    SceCtrlPadInputMode input_mode = SCE_CTRL_MODE_DIGITAL;
    SceCtrlPadInputMode input_mode_ext = SCE_CTRL_MODE_DIGITAL;

    // last vsync the data was read
    uint64_t last_vcount[5] = {}; // sceCtrl ports.

    VirtualKeyboardState keyboard_state;
    // buttons and sticks held by an external test driver (VITA3K_INPUT_PIPE: a named pipe on Windows, a FIFO elsewhere)
    VirtualKeyboardState injected_state;
    // buttons and sticks held on a frontend's on-screen controls (a phone's touchscreen), added to the others
    VirtualKeyboardState screen_state;
    // the right stick a frontend makes from the mouse while it captures the cursor (mouse camera), added to the others
    VirtualKeyboardState mouse_state;
    // relative mouse motion the test driver sent (in mouse counts), which the frontend takes as if the captured mouse moved
    float injected_mouse_dx = 0, injected_mouse_dy = 0;
    // PNG paths the test driver asked to save the current frame to (the frontend takes them)
    std::vector<std::string> injected_screenshots;
    // (path, guest address, size) dumps of guest memory the test driver asked for (the frontend writes them)
    std::vector<std::tuple<std::string, uint32_t, uint32_t>> injected_memdumps;
    // (guest address, bytes) writes to guest memory the test driver asked for (the frontend makes them)
    std::vector<std::pair<uint32_t, std::vector<uint8_t>>> injected_memwrites;
    // the test driver asked to open or close a front end's settings menu
    bool injected_menu_toggle = false;
    // UTF-8 text the test driver types into the game's keyboard dialog (the frontend commits it while one is
    // open), and Enter pressed on that dialog after it
    std::vector<std::string> injected_texts;
    bool injected_text_submit = false;
    // SDL key names (F5, F12 ...) the test driver presses and releases, as the window's own key events (the
    // frontend pushes them into SDL's queue): a frontend's own keys without the window's focus
    std::vector<std::string> injected_keys;
    // (frame count, folder) recordings the test driver asked for
    std::vector<std::pair<int, std::string>> injected_records;
    // front-touch fingers the test driver holds, in 960x544 screen points (the frontend applies them)
    std::vector<std::pair<float, float>> injected_touches;
    // the gamepad inputs the test driver holds now (SDL's names: south, dpup, righttrigger ...), set
    // until the frontend takes them onto a virtual gamepad of its own
    std::optional<std::vector<std::string>> injected_gamepad;

    std::atomic<bool> overlay_input_intercepted{ false };

    struct OverlayMouseState {
        std::atomic<float> x{ 0.f };
        std::atomic<float> y{ 0.f };
        std::atomic<bool> pressed{ false };
    };
    OverlayMouseState overlay_mouse;

    void reset_runtime() {
        controllers.clear();
        controllers_num = 0;
        has_motion_support = false;
        std::fill_n(free_ports, SCE_CTRL_MAX_WIRELESS_NUM, true);
        input_mode = SCE_CTRL_MODE_DIGITAL;
        input_mode_ext = SCE_CTRL_MODE_DIGITAL;
        std::fill_n(last_vcount, 5, 0);
        keyboard_state = {};
        screen_state = {};
        mouse_state = {};
        overlay_input_intercepted.store(false, std::memory_order_relaxed);
        overlay_mouse.x.store(0.f, std::memory_order_relaxed);
        overlay_mouse.y.store(0.f, std::memory_order_relaxed);
        overlay_mouse.pressed.store(false, std::memory_order_relaxed);
    }
};
