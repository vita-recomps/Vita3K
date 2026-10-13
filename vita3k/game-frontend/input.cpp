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

#include "input.h"

#include <config/state.h>
#include <ctrl/ctrl.h>
#include <ctrl/state.h>
#include <emuenv/state.h>
#include <touch/state.h>
#include <util/string_utils.h>

#include <SDL3/SDL_gamepad.h>

#include <algorithm>
#include <cctype>
#include <mutex>

namespace game_frontend {

using input::PhysicalKeyCode;

namespace {

std::string trim(const std::string &s) {
    const auto b = s.find_first_not_of(" \t");
    if (b == std::string::npos)
        return {};
    return s.substr(b, s.find_last_not_of(" \t") - b + 1);
}

std::vector<std::string> split_list(const std::string &s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= s.size()) {
        const auto comma = s.find(',', start);
        const auto item = trim(s.substr(start, comma == std::string::npos ? std::string::npos : comma - start));
        if (!item.empty())
            out.push_back(item);
        if (comma == std::string::npos)
            break;
        start = comma + 1;
    }
    return out;
}

// A key by its W3C code (KeyX, ArrowUp, ShiftRight ...), a single letter or digit, or the names
// SDL uses (Up, Return, Left Shift, Keypad 1 ...)
PhysicalKeyCode parse_key(const std::string &name) {
    if (const auto code = input::physical_key_from_code_name(name); code != PhysicalKeyCode::Unbound)
        return code;
    if (name.size() == 1 && std::isalpha(static_cast<unsigned char>(name[0])))
        return input::physical_key_from_code_name(std::string("Key") + static_cast<char>(std::toupper(static_cast<unsigned char>(name[0]))));
    if (name.size() == 1 && std::isdigit(static_cast<unsigned char>(name[0])))
        return input::physical_key_from_code_name(std::string("Digit") + name);
    const std::string lower = string_utils::tolower(name);
    static const std::map<std::string, std::string> aliases{
        { "up", "ArrowUp" }, { "down", "ArrowDown" }, { "left", "ArrowLeft" }, { "right", "ArrowRight" },
        { "return", "Enter" }, { "enter", "Enter" }, { "space", "Space" }, { "tab", "Tab" },
        { "backspace", "Backspace" }, { "escape", "Escape" }, { "esc", "Escape" },
        { "left shift", "ShiftLeft" }, { "right shift", "ShiftRight" }, { "lshift", "ShiftLeft" }, { "rshift", "ShiftRight" },
        { "left ctrl", "ControlLeft" }, { "right ctrl", "ControlRight" }, { "lctrl", "ControlLeft" }, { "rctrl", "ControlRight" },
        { "left alt", "AltLeft" }, { "right alt", "AltRight" }, { "lalt", "AltLeft" }, { "ralt", "AltRight" },
        { "keypad enter", "NumpadEnter" },
    };
    if (const auto it = aliases.find(lower); it != aliases.end())
        return input::physical_key_from_code_name(it->second);
    if (lower.starts_with("keypad ") && lower.size() == 8 && std::isdigit(static_cast<unsigned char>(lower[7])))
        return input::physical_key_from_code_name(std::string("Numpad") + lower[7]);
    if (lower.size() >= 2 && lower[0] == 'f')
        return input::physical_key_from_code_name("F" + lower.substr(1));
    return PhysicalKeyCode::Unbound;
}

SDL_GamepadButton parse_gamepad_button(const std::string &name) {
    static const std::map<std::string, SDL_GamepadButton> names{
        { "south", SDL_GAMEPAD_BUTTON_SOUTH }, { "east", SDL_GAMEPAD_BUTTON_EAST },
        { "west", SDL_GAMEPAD_BUTTON_WEST }, { "north", SDL_GAMEPAD_BUTTON_NORTH },
    };
    const std::string lower = string_utils::tolower(name);
    if (const auto it = names.find(lower); it != names.end())
        return it->second;
    return SDL_GetGamepadButtonFromString(lower.c_str());
}

} // namespace

void apply_bindings(Config &cfg, const std::map<std::string, std::string> &keyboard,
    const std::map<std::string, std::string> &gamepad, std::vector<std::string> &warnings) {
    struct KeyBinding {
        PhysicalKeyCode Config::*primary;
        PhysicalKeyCode Config::*alternate;
    };
    static const std::map<std::string, KeyBinding> keys{
        { "cross", { &Config::keyboard_button_cross, &Config::keyboard_button_cross_alt } },
        { "circle", { &Config::keyboard_button_circle, &Config::keyboard_button_circle_alt } },
        { "square", { &Config::keyboard_button_square, &Config::keyboard_button_square_alt } },
        { "triangle", { &Config::keyboard_button_triangle, &Config::keyboard_button_triangle_alt } },
        { "l", { &Config::keyboard_button_l1, &Config::keyboard_button_l1_alt } },
        { "r", { &Config::keyboard_button_r1, &Config::keyboard_button_r1_alt } },
        { "start", { &Config::keyboard_button_start, &Config::keyboard_button_start_alt } },
        { "select", { &Config::keyboard_button_select, &Config::keyboard_button_select_alt } },
        { "up", { &Config::keyboard_button_up, &Config::keyboard_button_up_alt } },
        { "down", { &Config::keyboard_button_down, &Config::keyboard_button_down_alt } },
        { "left", { &Config::keyboard_button_left, &Config::keyboard_button_left_alt } },
        { "right", { &Config::keyboard_button_right, &Config::keyboard_button_right_alt } },
        { "analog_up", { &Config::keyboard_leftstick_up, &Config::keyboard_leftstick_up_alt } },
        { "analog_down", { &Config::keyboard_leftstick_down, &Config::keyboard_leftstick_down_alt } },
        { "analog_left", { &Config::keyboard_leftstick_left, &Config::keyboard_leftstick_left_alt } },
        { "analog_right", { &Config::keyboard_leftstick_right, &Config::keyboard_leftstick_right_alt } },
        { "right_analog_up", { &Config::keyboard_rightstick_up, &Config::keyboard_rightstick_up_alt } },
        { "right_analog_down", { &Config::keyboard_rightstick_down, &Config::keyboard_rightstick_down_alt } },
        { "right_analog_left", { &Config::keyboard_rightstick_left, &Config::keyboard_rightstick_left_alt } },
        { "right_analog_right", { &Config::keyboard_rightstick_right, &Config::keyboard_rightstick_right_alt } },
    };
    for (const auto &[name, value] : keyboard) {
        const auto it = keys.find(name);
        if (it == keys.end()) {
            warnings.push_back("keyboard." + name + " (no such Vita button)");
            continue;
        }
        const auto list = split_list(value);
        std::vector<PhysicalKeyCode> codes;
        for (const auto &key : list) {
            const auto code = parse_key(key);
            if (code == PhysicalKeyCode::Unbound)
                warnings.push_back("keyboard." + name + ": unknown key \"" + key + "\"");
            else
                codes.push_back(code);
        }
        if (codes.empty() && !list.empty())
            continue; // nothing usable: keep the default rather than unbind the button
        if (codes.size() > 2)
            warnings.push_back("keyboard." + name + ": only two keys per button, the rest are ignored");
        cfg.*(it->second.primary) = codes.size() > 0 ? codes[0] : PhysicalKeyCode::Unbound;
        cfg.*(it->second.alternate) = codes.size() > 1 ? codes[1] : PhysicalKeyCode::Unbound;
    }

    // Vita3K's gamepad bindings: controller_binds[slot] is the gamepad button that presses the
    // Vita button the slot stands for
    static const std::map<std::string, SDL_GamepadButton> slots{
        { "cross", SDL_GAMEPAD_BUTTON_SOUTH }, { "circle", SDL_GAMEPAD_BUTTON_EAST },
        { "square", SDL_GAMEPAD_BUTTON_WEST }, { "triangle", SDL_GAMEPAD_BUTTON_NORTH },
        { "l", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER }, { "r", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER },
        { "start", SDL_GAMEPAD_BUTTON_START }, { "select", SDL_GAMEPAD_BUTTON_BACK },
        { "up", SDL_GAMEPAD_BUTTON_DPAD_UP }, { "down", SDL_GAMEPAD_BUTTON_DPAD_DOWN },
        { "left", SDL_GAMEPAD_BUTTON_DPAD_LEFT }, { "right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT },
    };
    for (const auto &[name, value] : gamepad) {
        const auto it = slots.find(name);
        if (it == slots.end()) {
            warnings.push_back("gamepad." + name + " (no such Vita button)");
            continue;
        }
        const auto list = split_list(value);
        if (list.empty())
            continue;
        if (list.size() > 1)
            warnings.push_back("gamepad." + name + ": one gamepad button per Vita button, the rest are ignored");
        const SDL_GamepadButton button = parse_gamepad_button(list[0]);
        if (button == SDL_GAMEPAD_BUTTON_INVALID) {
            warnings.push_back("gamepad." + name + ": unknown gamepad button \"" + list[0] + "\"");
            continue;
        }
        if (static_cast<size_t>(it->second) < cfg.controller_binds.size())
            cfg.controller_binds[it->second] = static_cast<short>(button);
    }
}

void Keyboard::key(EmuEnvState &emuenv, int scancode, bool is_pressed) {
    // SDL scancodes are USB HID keyboard usages, as are the physical key codes
    const auto code = static_cast<PhysicalKeyCode>(0x070000 | (scancode & 0xffff));
    if (input::physical_key_code_name(code).empty())
        return;
    if (is_pressed)
        pressed.insert(code);
    else
        pressed.erase(code);
    update(emuenv);
}

void Keyboard::release_all(EmuEnvState &emuenv) {
    pressed.clear();
    update(emuenv);
}

// As the Qt interface does it (gui-qt/src/ctrl_keyboard_filter.cpp)
void Keyboard::update(EmuEnvState &emuenv) {
    const auto &cfg = emuenv.cfg;
    const auto down = [this](PhysicalKeyCode primary, PhysicalKeyCode alternate) {
        return (primary != PhysicalKeyCode::Unbound && pressed.contains(primary))
            || (alternate != PhysicalKeyCode::Unbound && pressed.contains(alternate));
    };
    const auto axis = [&](PhysicalKeyCode neg, PhysicalKeyCode neg_alt, PhysicalKeyCode pos, PhysicalKeyCode pos_alt) {
        return static_cast<float>(static_cast<int>(down(pos, pos_alt)) - static_cast<int>(down(neg, neg_alt)));
    };

    uint32_t buttons = 0, buttons_ext = 0;
    const auto set = [&](PhysicalKeyCode primary, PhysicalKeyCode alternate, uint32_t bit, uint32_t &mask) {
        if (down(primary, alternate))
            mask |= bit;
    };
    for (uint32_t *mask : { &buttons, &buttons_ext }) {
        set(cfg.keyboard_button_select, cfg.keyboard_button_select_alt, SCE_CTRL_SELECT, *mask);
        set(cfg.keyboard_button_start, cfg.keyboard_button_start_alt, SCE_CTRL_START, *mask);
        set(cfg.keyboard_button_up, cfg.keyboard_button_up_alt, SCE_CTRL_UP, *mask);
        set(cfg.keyboard_button_right, cfg.keyboard_button_right_alt, SCE_CTRL_RIGHT, *mask);
        set(cfg.keyboard_button_down, cfg.keyboard_button_down_alt, SCE_CTRL_DOWN, *mask);
        set(cfg.keyboard_button_left, cfg.keyboard_button_left_alt, SCE_CTRL_LEFT, *mask);
        set(cfg.keyboard_button_triangle, cfg.keyboard_button_triangle_alt, SCE_CTRL_TRIANGLE, *mask);
        set(cfg.keyboard_button_circle, cfg.keyboard_button_circle_alt, SCE_CTRL_CIRCLE, *mask);
        set(cfg.keyboard_button_cross, cfg.keyboard_button_cross_alt, SCE_CTRL_CROSS, *mask);
        set(cfg.keyboard_button_square, cfg.keyboard_button_square_alt, SCE_CTRL_SQUARE, *mask);
    }
    set(cfg.keyboard_button_l1, cfg.keyboard_button_l1_alt, SCE_CTRL_L, buttons);
    set(cfg.keyboard_button_r1, cfg.keyboard_button_r1_alt, SCE_CTRL_R, buttons);
    set(cfg.keyboard_button_l1, cfg.keyboard_button_l1_alt, SCE_CTRL_L1, buttons_ext);
    set(cfg.keyboard_button_r1, cfg.keyboard_button_r1_alt, SCE_CTRL_R1, buttons_ext);

    const std::lock_guard<std::mutex> lock(emuenv.ctrl.mutex);
    auto &kb = emuenv.ctrl.keyboard_state;
    kb.buttons = buttons;
    kb.buttons_ext = buttons_ext;
    kb.axes[0] = axis(cfg.keyboard_leftstick_left, cfg.keyboard_leftstick_left_alt, cfg.keyboard_leftstick_right, cfg.keyboard_leftstick_right_alt);
    kb.axes[1] = axis(cfg.keyboard_leftstick_up, cfg.keyboard_leftstick_up_alt, cfg.keyboard_leftstick_down, cfg.keyboard_leftstick_down_alt);
    kb.axes[2] = axis(cfg.keyboard_rightstick_left, cfg.keyboard_rightstick_left_alt, cfg.keyboard_rightstick_right, cfg.keyboard_rightstick_right_alt);
    kb.axes[3] = axis(cfg.keyboard_rightstick_up, cfg.keyboard_rightstick_up_alt, cfg.keyboard_rightstick_down, cfg.keyboard_rightstick_down_alt);
}

void Touches::configure(const std::vector<TouchBinding> &touches, std::vector<std::string> &warnings) {
    bindings.clear();
    for (const auto &touch : touches) {
        Binding b{ touch.x, touch.y };
        for (const auto &key : split_list(touch.keys)) {
            const auto code = parse_key(key);
            if (code == PhysicalKeyCode::Unbound)
                warnings.push_back("touch." + touch.name + "_key: unknown key \"" + key + "\"");
            else if (b.keys.size() < 2)
                b.keys.push_back(code);
        }
        const std::string pad = string_utils::tolower(trim(touch.gamepad));
        if (pad == "lefttrigger")
            b.gamepad_axis = SDL_GAMEPAD_AXIS_LEFT_TRIGGER;
        else if (pad == "righttrigger")
            b.gamepad_axis = SDL_GAMEPAD_AXIS_RIGHT_TRIGGER;
        else if (!pad.empty()) {
            const SDL_GamepadButton button = parse_gamepad_button(pad);
            if (button == SDL_GAMEPAD_BUTTON_INVALID)
                warnings.push_back("touch." + touch.name + "_gamepad: unknown gamepad button \"" + touch.gamepad + "\"");
            else
                b.gamepad_button = button;
        }
        bindings.push_back(std::move(b));
    }
}

void Touches::update(EmuEnvState &emuenv, const Keyboard &keyboard) {
    // the touch panel's units: the front panel is 1920x1088 over the 960x544 screen
    const auto at = [](float x, float y, uint8_t id) {
        return TouchState::HeldTouch{ static_cast<uint16_t>(x * 2), static_cast<uint16_t>(y * 2), id };
    };
    std::vector<TouchState::HeldTouch> held;
    {
        const std::lock_guard<std::mutex> lock(emuenv.ctrl.mutex);
        for (size_t i = 0; i < bindings.size(); i++) {
            const auto &b = bindings[i];
            bool down = std::any_of(b.keys.begin(), b.keys.end(), [&](PhysicalKeyCode k) { return keyboard.down(k); });
            for (const auto &[id, controller] : emuenv.ctrl.controllers) {
                SDL_Gamepad *pad = controller.controller.get();
                if (!pad)
                    continue;
                if (b.gamepad_button >= 0 && SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(b.gamepad_button)))
                    down = true;
                if (b.gamepad_axis >= 0 && SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(b.gamepad_axis)) > 16384)
                    down = true;
            }
            // ids that stay the same while a finger is down, apart from the mouse's (0-127)
            if (down)
                held.push_back(at(b.x, b.y, static_cast<uint8_t>(100 + i % 16)));
        }
        for (size_t i = 0; i < emuenv.ctrl.injected_touches.size(); i++) {
            const auto &[x, y] = emuenv.ctrl.injected_touches[i];
            held.push_back(at(x, y, static_cast<uint8_t>(120 + i % 8)));
        }
    }
    const std::lock_guard<std::mutex> lock(emuenv.touch.held_mutex);
    emuenv.touch.held[SCE_TOUCH_PORT_FRONT] = std::move(held);
}

} // namespace game_frontend
