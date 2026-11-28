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
#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <cmath>
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
    return split_bindings(s);
}

// the mouse buttons a binding can name (mouse_button_code), by SDL_BUTTON_*
constexpr uint32_t MOUSE_PAGE = 0x090000;
const std::pair<int, const char *> mouse_names[] = { { 1, "MouseLeft" }, { 3, "MouseRight" }, { 4, "MouseX1" }, { 5, "MouseX2" } };

// A key by its W3C code (KeyX, ArrowUp, ShiftRight ...), a single letter or digit, the names SDL uses
// (Up, Return, Left Shift, Keypad 1 ...), or a mouse button (MouseX1, Mouse4 ...)
PhysicalKeyCode parse_key(const std::string &name) {
    if (const auto code = input::physical_key_from_code_name(name); code != PhysicalKeyCode::Unbound)
        return code;
    {
        const std::string lower = string_utils::tolower(name);
        for (const auto &[button, mouse] : mouse_names)
            if (lower == string_utils::tolower(mouse) || lower == "mouse" + std::to_string(button))
                return mouse_button_code(button);
    }
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
    return static_cast<SDL_GamepadButton>(gamepad_button_from_name(name));
}

// a gamepad button or a trigger, as [gamepad] and [touch] name them
struct GamepadInput {
    int button = -1; // SDL_GamepadButton
    int axis = -1; // SDL_GamepadAxis (a trigger)
    bool operator==(const GamepadInput &) const = default;
};
std::optional<GamepadInput> parse_gamepad_input(const std::string &name) {
    const std::string lower = string_utils::tolower(trim(name));
    if (lower == "lefttrigger")
        return GamepadInput{ -1, SDL_GAMEPAD_AXIS_LEFT_TRIGGER };
    if (lower == "righttrigger")
        return GamepadInput{ -1, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER };
    const SDL_GamepadButton button = parse_gamepad_button(lower);
    if (button == SDL_GAMEPAD_BUTTON_INVALID)
        return std::nullopt;
    return GamepadInput{ button, -1 };
}

// up to two gamepad inputs from a list, a repeated one left out; an unknown name adds "<setting>:
// unknown gamepad button" to `warnings`, a third "<setting>: only two ..."
std::vector<GamepadInput> parse_gamepad_inputs(const std::vector<std::string> &list, const std::string &setting, std::vector<std::string> &warnings) {
    std::vector<GamepadInput> inputs;
    bool dropped = false;
    for (const auto &name : list) {
        const auto input = parse_gamepad_input(name);
        if (!input)
            warnings.push_back(setting + ": unknown gamepad button \"" + name + "\"");
        else if (std::find(inputs.begin(), inputs.end(), *input) != inputs.end())
            continue;
        else if (inputs.size() < 2)
            inputs.push_back(*input);
        else
            dropped = true;
    }
    if (dropped)
        warnings.push_back(setting + ": only two gamepad buttons, the rest are ignored");
    return inputs;
}

} // namespace

const std::vector<ButtonBinding> &button_bindings() {
    static const std::vector<ButtonBinding> bindings{
        { "cross", "Cross", "X", "south", "" },
        { "circle", "Circle", "C", "east", "" },
        { "square", "Square", "Z", "west", "" },
        { "triangle", "Triangle", "V", "north", "" },
        { "l", "L", "Q", "leftshoulder", "" },
        { "r", "R", "E", "rightshoulder", "" },
        { "start", "Start", "Return", "start", "" },
        { "select", "Select", "Right Shift", "back", "" },
        { "up", "D-pad up", "Up", "dpup", "" },
        { "down", "D-pad down", "Down", "dpdown", "" },
        { "left", "D-pad left", "Left", "dpleft", "" },
        { "right", "D-pad right", "Right", "dpright", "" },
        { "analog_up", "Left stick up", "W", "", "Left stick" },
        { "analog_down", "Left stick down", "S", "", "Left stick" },
        { "analog_left", "Left stick left", "A", "", "Left stick" },
        { "analog_right", "Left stick right", "D", "", "Left stick" },
        { "right_analog_up", "Right stick up", "I", "", "Right stick" },
        { "right_analog_down", "Right stick down", "K", "", "Right stick" },
        { "right_analog_left", "Right stick left", "J", "", "Right stick" },
        { "right_analog_right", "Right stick right", "L", "", "Right stick" },
        // a PS TV's DualShock: the triggers always press them (ctrl.cpp, apply_controller)
        { "l2", "L2 (PS TV)", "U", "", "LT", false },
        { "r2", "R2 (PS TV)", "O", "", "RT", false },
    };
    return bindings;
}

std::string default_keyboard_lines() {
    std::string out;
    for (const auto &b : button_bindings())
        if (b.in_default_file)
            out += fmt::format("{} = {}\n", b.name, b.keys);
    return out;
}

std::string default_gamepad_lines() {
    std::string out;
    for (const auto &b : button_bindings())
        if (*b.gamepad)
            out += fmt::format("{} = {}\n", b.name, b.gamepad);
    return out;
}

std::vector<std::string> split_bindings(const std::string &s) {
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

PhysicalKeyCode mouse_button_code(int sdl_button) {
    return static_cast<PhysicalKeyCode>(MOUSE_PAGE | (sdl_button & 0xff));
}

PhysicalKeyCode key_from_name(const std::string &name) {
    return parse_key(trim(name));
}

std::string key_name(PhysicalKeyCode code) {
    for (const auto &[button, mouse] : mouse_names)
        if (code == mouse_button_code(button))
            return mouse;
    const std::string w3c(input::physical_key_code_name(code));
    if (w3c.empty())
        return {};
    if (w3c.size() == 4 && w3c.starts_with("Key"))
        return w3c.substr(3);
    if (w3c.size() == 6 && w3c.starts_with("Digit"))
        return w3c.substr(5);
    if (w3c.starts_with("Arrow"))
        return w3c.substr(5);
    if (w3c.size() == 7 && w3c.starts_with("Numpad") && std::isdigit(static_cast<unsigned char>(w3c[6])))
        return "Keypad " + w3c.substr(6);
    static const std::map<std::string, std::string> names{
        { "ShiftLeft", "Left Shift" }, { "ShiftRight", "Right Shift" }, { "ControlLeft", "Left Ctrl" },
        { "ControlRight", "Right Ctrl" }, { "AltLeft", "Left Alt" }, { "AltRight", "Right Alt" },
        { "NumpadEnter", "Keypad Enter" },
    };
    if (const auto it = names.find(w3c); it != names.end())
        return it->second;
    return w3c; // F1, Enter, Space, Tab, Backspace, Escape, Comma ...: the W3C code reads back as it is
}

std::string key_cap_text(PhysicalKeyCode code) {
    static const std::map<std::string, std::string> short_forms{
        { "Space", "SPC" }, { "Enter", "ENT" }, { "NumpadEnter", "ENT" }, { "Tab", "TAB" }, { "Backspace", "BS" },
        { "Escape", "ESC" }, { "ShiftLeft", "SHF" }, { "ShiftRight", "SHF" }, { "ControlLeft", "CTL" },
        { "ControlRight", "CTL" }, { "AltLeft", "ALT" }, { "AltRight", "ALT" },
#ifdef __APPLE__
        { "MetaLeft", "CMD" }, { "MetaRight", "CMD" },
#else
        { "MetaLeft", "WIN" }, { "MetaRight", "WIN" },
#endif
        { "CapsLock", "CAP" }, { "Insert", "INS" }, { "Delete", "DEL" }, { "Home", "HOM" },
        { "End", "END" }, { "PageUp", "PGU" }, { "PageDown", "PGD" }, { "Comma", "," }, { "Period", "." },
        { "Slash", "/" }, { "Backslash", "\\" }, { "Semicolon", ";" }, { "Quote", "'" }, { "BracketLeft", "[" },
        { "BracketRight", "]" }, { "Minus", "-" }, { "Equal", "=" }, { "Backquote", "`" }, { "NumpadAdd", "+" },
        { "NumpadSubtract", "-" }, { "NumpadMultiply", "*" }, { "NumpadDivide", "/" }, { "NumpadDecimal", "." },
        { "ArrowUp", std::string(1, KEYCAP_UP) }, { "ArrowDown", std::string(1, KEYCAP_DOWN) },
        { "ArrowLeft", std::string(1, KEYCAP_LEFT) }, { "ArrowRight", std::string(1, KEYCAP_RIGHT) },
    };
    static const std::map<int, std::string> mouse_forms{ { 1, "LMB" }, { 3, "RMB" }, { 4, "M4" }, { 5, "M5" } };
    for (const auto &[button, text] : mouse_forms)
        if (code == mouse_button_code(button))
            return text;
    const std::string w3c(input::physical_key_code_name(code));
    if (const auto it = short_forms.find(w3c); it != short_forms.end())
        return it->second;
    const std::string name = key_name(code);
    if (name.starts_with("Keypad "))
        return name.substr(7);
    if (name.size() <= 3)
        return string_utils::toupper(name); // X, 1, F5, F12
    // anything else: its first letters
    std::string out;
    for (const char c : name)
        if (std::isalnum(static_cast<unsigned char>(c)) && out.size() < 3)
            out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

int gamepad_button_from_name(const std::string &name) {
    static const std::map<std::string, SDL_GamepadButton> names{
        { "south", SDL_GAMEPAD_BUTTON_SOUTH }, { "east", SDL_GAMEPAD_BUTTON_EAST },
        { "west", SDL_GAMEPAD_BUTTON_WEST }, { "north", SDL_GAMEPAD_BUTTON_NORTH },
    };
    const std::string lower = string_utils::tolower(trim(name));
    if (const auto it = names.find(lower); it != names.end())
        return it->second;
    return SDL_GetGamepadButtonFromString(lower.c_str());
}

std::string gamepad_button_name(int button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return "south";
    case SDL_GAMEPAD_BUTTON_EAST: return "east";
    case SDL_GAMEPAD_BUTTON_WEST: return "west";
    case SDL_GAMEPAD_BUTTON_NORTH: return "north";
    default: break;
    }
    const char *name = SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(button));
    return name ? name : "";
}

std::string gamepad_display_name(const std::string &name) {
    const std::string lower = string_utils::tolower(trim(name));
    static const std::map<std::string, std::string> labels{
        { "south", "A" }, { "a", "A" }, { "east", "B" }, { "b", "B" }, { "west", "X" }, { "x", "X" },
        { "north", "Y" }, { "y", "Y" }, { "leftshoulder", "LB" }, { "rightshoulder", "RB" },
        { "lefttrigger", "LT" }, { "righttrigger", "RT" }, { "start", "Menu" }, { "back", "View" },
        { "guide", "Guide" }, { "leftstick", "LS click" }, { "rightstick", "RS click" }, { "dpup", "D-pad up" },
        { "dpdown", "D-pad down" }, { "dpleft", "D-pad left" }, { "dpright", "D-pad right" }, { "misc1", "Share" },
        { "touchpad", "Touchpad" }, { "paddle1", "Paddle 1" }, { "paddle2", "Paddle 2" }, { "paddle3", "Paddle 3" },
        { "paddle4", "Paddle 4" },
    };
    if (const auto it = labels.find(lower); it != labels.end())
        return it->second;
    std::string out = trim(name);
    if (!out.empty())
        out[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(out[0])));
    return out;
}

void apply_bindings(EmuEnvState &emuenv, const std::map<std::string, std::string> &keyboard,
    const std::map<std::string, std::string> &gamepad, std::vector<std::string> &warnings) {
    Config &cfg = emuenv.cfg;
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
        { "l2", { &Config::keyboard_button_l2, &Config::keyboard_button_l2_alt } },
        { "r2", { &Config::keyboard_button_r2, &Config::keyboard_button_r2_alt } },
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

    // Vita3K's gamepad bindings: controller_binds[slot] is the gamepad button that presses the Vita
    // button the slot stands for. It takes one button: a second one, and a trigger, go into the
    // controller's extra bindings (ExtraGamepadBinding), with the Vita button's bits.
    struct Slot {
        SDL_GamepadButton slot;
        uint32_t vita_button, vita_button_ext;
    };
    static const std::map<std::string, Slot> slots{
        { "cross", { SDL_GAMEPAD_BUTTON_SOUTH, SCE_CTRL_CROSS, SCE_CTRL_CROSS } },
        { "circle", { SDL_GAMEPAD_BUTTON_EAST, SCE_CTRL_CIRCLE, SCE_CTRL_CIRCLE } },
        { "square", { SDL_GAMEPAD_BUTTON_WEST, SCE_CTRL_SQUARE, SCE_CTRL_SQUARE } },
        { "triangle", { SDL_GAMEPAD_BUTTON_NORTH, SCE_CTRL_TRIANGLE, SCE_CTRL_TRIANGLE } },
        { "l", { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SCE_CTRL_L, SCE_CTRL_L1 } },
        { "r", { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, SCE_CTRL_R, SCE_CTRL_R1 } },
        { "start", { SDL_GAMEPAD_BUTTON_START, SCE_CTRL_START, SCE_CTRL_START } },
        { "select", { SDL_GAMEPAD_BUTTON_BACK, SCE_CTRL_SELECT, SCE_CTRL_SELECT } },
        { "up", { SDL_GAMEPAD_BUTTON_DPAD_UP, SCE_CTRL_UP, SCE_CTRL_UP } },
        { "down", { SDL_GAMEPAD_BUTTON_DPAD_DOWN, SCE_CTRL_DOWN, SCE_CTRL_DOWN } },
        { "left", { SDL_GAMEPAD_BUTTON_DPAD_LEFT, SCE_CTRL_LEFT, SCE_CTRL_LEFT } },
        { "right", { SDL_GAMEPAD_BUTTON_DPAD_RIGHT, SCE_CTRL_RIGHT, SCE_CTRL_RIGHT } },
    };
    if (gamepad.empty())
        return;
    {
        const std::lock_guard lock(emuenv.ctrl.mutex);
        auto &binds = emuenv.ctrl.extra_gamepad_binds;
        for (const auto &[name, value] : gamepad) {
            const auto it = slots.find(name);
            if (it == slots.end()) {
                warnings.push_back("gamepad." + name + " (no such Vita button)");
                continue;
            }
            const Slot &slot = it->second;
            const auto list = split_list(value);
            const auto inputs = parse_gamepad_inputs(list, "gamepad." + name, warnings);
            if (inputs.empty() && !list.empty())
                continue; // nothing usable: keep the binding rather than unbind the button
            // the first button stays Vita3K's; everything else (an empty list: nothing) is extra
            int first = SDL_GAMEPAD_BUTTON_INVALID;
            std::vector<ExtraGamepadBinding> extra;
            for (const auto &input : inputs) {
                if (input.button >= 0 && first == SDL_GAMEPAD_BUTTON_INVALID)
                    first = input.button;
                else
                    extra.push_back({ input.button, input.axis, slot.vita_button, slot.vita_button_ext });
            }
            if (static_cast<size_t>(slot.slot) < cfg.controller_binds.size())
                cfg.controller_binds[slot.slot] = static_cast<short>(first);
            std::erase_if(binds, [&](const ExtraGamepadBinding &b) { return b.vita_button == slot.vita_button && b.vita_button_ext == slot.vita_button_ext; });
            binds.insert(binds.end(), extra.begin(), extra.end());
        }
    }
    sync_claimed_gamepad_buttons(emuenv);
}

void sync_claimed_gamepad_buttons(EmuEnvState &emuenv, const std::vector<short> &touch_buttons) {
    static constexpr SDL_GamepadButton managed_slots[] = {
        SDL_GAMEPAD_BUTTON_SOUTH, SDL_GAMEPAD_BUTTON_EAST, SDL_GAMEPAD_BUTTON_WEST, SDL_GAMEPAD_BUTTON_NORTH,
        SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
        SDL_GAMEPAD_BUTTON_START, SDL_GAMEPAD_BUTTON_BACK,
        SDL_GAMEPAD_BUTTON_DPAD_UP, SDL_GAMEPAD_BUTTON_DPAD_DOWN, SDL_GAMEPAD_BUTTON_DPAD_LEFT, SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
    };
    const std::lock_guard lock(emuenv.ctrl.mutex);
    auto &claimed = emuenv.ctrl.claimed_gamepad_buttons;
    claimed.clear();
    const auto &binds = emuenv.cfg.controller_binds;
    for (const SDL_GamepadButton slot : managed_slots) {
        if (static_cast<size_t>(slot) < binds.size() && binds[slot] >= 0)
            claimed.push_back(binds[slot]);
    }
    for (const auto &extra : emuenv.ctrl.extra_gamepad_binds) {
        if (extra.button >= 0)
            claimed.push_back(static_cast<short>(extra.button));
    }
    for (const short button : touch_buttons) {
        if (button >= 0)
            claimed.push_back(button);
    }
}

PhysicalKeyCode key_from_scancode(int scancode) {
    return static_cast<PhysicalKeyCode>(0x070000 | (scancode & 0xffff));
}

std::vector<PhysicalKeyCode> parse_keys(const std::string &list, const std::string &setting, std::vector<std::string> &warnings) {
    std::vector<PhysicalKeyCode> codes;
    for (const auto &key : split_list(list)) {
        const auto code = parse_key(key);
        if (code == PhysicalKeyCode::Unbound)
            warnings.push_back(setting + ": unknown key \"" + key + "\"");
        else if (codes.size() < 2)
            codes.push_back(code);
    }
    return codes;
}

void Keyboard::key(EmuEnvState &emuenv, int scancode, bool is_pressed) {
    const auto code = key_from_scancode(scancode);
    if (input::physical_key_code_name(code).empty())
        return;
    if (is_pressed)
        pressed.insert(code);
    else
        pressed.erase(code);
    update(emuenv);
}

void Keyboard::mouse_button(EmuEnvState &emuenv, int sdl_button, bool is_pressed) {
    const auto code = mouse_button_code(sdl_button);
    if (is_pressed == pressed.contains(code))
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
    // L2/R2 (a PS TV's DualShock) exist only in the extended functions: the basic ones report the
    // Vita's L/R in the same bits
    set(cfg.keyboard_button_l2, cfg.keyboard_button_l2_alt, SCE_CTRL_L2, buttons_ext);
    set(cfg.keyboard_button_r2, cfg.keyboard_button_r2_alt, SCE_CTRL_R2, buttons_ext);

    const std::lock_guard<std::mutex> lock(emuenv.ctrl.mutex);
    auto &kb = emuenv.ctrl.keyboard_state;
    kb.buttons = buttons;
    kb.buttons_ext = buttons_ext;
    kb.axes[0] = axis(cfg.keyboard_leftstick_left, cfg.keyboard_leftstick_left_alt, cfg.keyboard_leftstick_right, cfg.keyboard_leftstick_right_alt);
    kb.axes[1] = axis(cfg.keyboard_leftstick_up, cfg.keyboard_leftstick_up_alt, cfg.keyboard_leftstick_down, cfg.keyboard_leftstick_down_alt);
    kb.axes[2] = axis(cfg.keyboard_rightstick_left, cfg.keyboard_rightstick_left_alt, cfg.keyboard_rightstick_right, cfg.keyboard_rightstick_right_alt);
    kb.axes[3] = axis(cfg.keyboard_rightstick_up, cfg.keyboard_rightstick_up_alt, cfg.keyboard_rightstick_down, cfg.keyboard_rightstick_down_alt);
}

float StickResponse::speed(float push) const {
    if (push <= points.front().first)
        return 0;
    for (size_t i = 1; i < points.size(); i++) {
        const auto &[p0, s0] = points[i - 1];
        const auto &[p1, s1] = points[i];
        if (push > p1)
            continue;
        if (p1 <= p0)
            return s1;
        const float t = (push - p0) / (p1 - p0);
        // a camera speed grows steeply with the push: exponentially between points (linearly from 0)
        return s0 > 0 && s1 > s0 ? s0 * std::pow(s1 / s0, t) : s0 + (s1 - s0) * t;
    }
    return points.back().second;
}

float StickResponse::push_for(float speed) const {
    if (speed <= 0)
        return 0;
    for (size_t i = 1; i < points.size(); i++) {
        const auto &[p0, s0] = points[i - 1];
        const auto &[p1, s1] = points[i];
        if (speed > s1)
            continue;
        if (s1 <= s0)
            return p0;
        const float t = s0 > 0 ? std::log(speed / s0) / std::log(s1 / s0) : (speed - s0) / (s1 - s0);
        return p0 + (p1 - p0) * t;
    }
    return points.back().first;
}

void MouseCamera::configure(const StickResponse &response_x, const StickResponse &response_y, const Options &options,
    CameraAngles angles, DirectTurn direct) {
    responses[0] = response_x;
    responses[1] = response_y;
    this->angles = std::move(angles);
    this->direct = std::move(direct);
    this->options = options;
}

void MouseCamera::motion(float dx, float dy) {
    const float degrees = degrees_per_count * options.sensitivity;
    axes[0].pending += dx * degrees;
    axes[1].pending += dy * degrees * (options.invert_y ? -1.0f : 1.0f);
}

float MouseCamera::stick_for(Axis &axis, const StickResponse &response, float dt) const {
    // what the stick turned since the last tick is no longer asked for; a turn past what was asked
    // ends the motion rather than turning back
    if (axis.pending != 0) {
        const float left = axis.pending - axis.speed * dt;
        axis.pending = left * axis.pending > 0 ? left : 0;
    }
    const float limit = response.max_speed() * max_backlog;
    axis.pending = std::clamp(axis.pending, -limit, limit);
    const auto stop = [&] {
        axis.pending = 0;
        axis.speed = 0;
        return 0.0f;
    };
    if (std::abs(axis.pending) < 0.01f)
        return stop();
    // the speed that turns the rest within catch_up, and the push that gives it
    const float push = response.push_for(std::abs(axis.pending) / catch_up);
    // The game reads the stick as a byte, 128 at the centre: the push reaches it as 128 +- steps. The
    // controller (ctrl.cpp, float_to_byte) truncates (v + 1) * 127.5, so +v came out a step short of
    // -v, and ss-delta, whose camera speed grows steeply with the push, turned the mouse's right turns
    // 15% slower than its left ones; the value returned lands on 128 +- steps exactly.
    float steps = std::round(push * 127.0f);
    if (response.speed(steps / 127.0f) <= 0)
        steps = std::ceil(push * 127.0f); // not back into the dead zone
    const float speed = response.speed(steps / 127.0f);
    if (speed <= 0)
        return stop();
    const float sign = axis.pending > 0 ? 1.0f : -1.0f;
    axis.speed = sign * speed;
    return (1.0f + sign * steps) / 127.5f;
}

float MouseCamera::stick_measured(Axis &axis, const StickResponse &response, float angle, float dt) const {
    float turned = 0;
    if (axis.angle) {
        turned = std::remainder(angle - *axis.angle, 360.0f);
        if (std::abs(turned) > max_turn_per_tick) {
            // another view: what was asked no longer applies to it
            turned = 0;
            axis.pending = 0;
        }
    }
    axis.angle = angle;
    // what the camera turned is no longer asked for; a turn past what was asked ends the motion
    if (axis.pending != 0 && turned != 0) {
        const float left = axis.pending - turned;
        axis.pending = left * axis.pending > 0 ? left : 0;
    }
    axis.per_tick = axis.held ? 0.7f * axis.per_tick + 0.3f * std::abs(turned) : 0.0f;
    axis.stalled = axis.held && turned == 0 ? axis.stalled + dt : 0.0f;
    if (axis.stalled > max_stall)
        axis.pending = 0;
    const float limit = response.max_speed() * max_backlog_measured;
    axis.pending = std::clamp(axis.pending, -limit, limit);
    // the camera stops as soon as the stick is let go: let go when what is left is less than about
    // half of what the next tick would turn
    if (std::abs(axis.pending) < std::max(0.05f, axis.per_tick * 0.5f)) {
        axis.pending = 0;
        axis.held = false;
        axis.stalled = 0;
        return 0;
    }
    axis.held = true;
    // the least push that turns at full speed, on a whole step from the centre (see stick_for)
    const float steps = std::ceil(response.push_for(response.max_speed()) * 127.0f - 0.001f);
    const float sign = axis.pending > 0 ? 1.0f : -1.0f;
    return (1.0f + sign * steps) / 127.5f;
}

void MouseCamera::update(EmuEnvState &emuenv, uint64_t now_ns) {
    const float dt = last_ns && now_ns > last_ns ? std::min(static_cast<float>(now_ns - last_ns) / 1e9f, 0.1f) : 0.0f;
    last_ns = now_ns;
    if (direct && direct(emuenv, axes[0].pending, axes[1].pending, now_ns)) {
        // the game's camera took the motion itself: the stick stays centred
        axes[0] = axes[1] = {};
        if (pushed) {
            pushed = false;
            const std::lock_guard<std::mutex> lock(emuenv.ctrl.mutex);
            emuenv.ctrl.mouse_state.axes[2] = 0;
            emuenv.ctrl.mouse_state.axes[3] = 0;
        }
        return;
    }
    std::optional<std::array<float, 2>> measured;
    if (angles)
        measured = angles(emuenv);
    if (!pushed && axes[0].pending == 0 && axes[1].pending == 0) {
        // keep following the camera, so that a motion counts from where it is
        for (int i = 0; i < 2; i++)
            axes[i].angle = measured ? std::optional<float>((*measured)[i]) : std::nullopt;
        return;
    }
    if (!measured)
        axes[0].angle = axes[1].angle = std::nullopt;
    const float x = measured ? stick_measured(axes[0], responses[0], (*measured)[0], dt) : stick_for(axes[0], responses[0], dt);
    const float y = measured ? stick_measured(axes[1], responses[1], (*measured)[1], dt) : stick_for(axes[1], responses[1], dt);
    const std::lock_guard<std::mutex> lock(emuenv.ctrl.mutex);
    auto &state = emuenv.ctrl.mouse_state;
    state.axes[2] = x;
    state.axes[3] = y;
    pushed = x != 0 || y != 0;
}

void MouseCamera::clear(EmuEnvState &emuenv) {
    axes[0] = axes[1] = {};
    pushed = false;
    const std::lock_guard<std::mutex> lock(emuenv.ctrl.mutex);
    emuenv.ctrl.mouse_state = {};
}

void Touches::configure(const std::vector<TouchBinding> &touches, std::vector<std::string> &warnings,
    const std::map<std::string, Target> &targets) {
    bindings.clear();
    for (const auto &touch : touches) {
        Binding b{ touch.x, touch.y };
        if (const auto it = targets.find(touch.name); it != targets.end())
            b.target = it->second;
        for (const auto &key : split_list(touch.keys)) {
            const auto code = parse_key(key);
            if (code == PhysicalKeyCode::Unbound)
                warnings.push_back("touch." + touch.name + "_key: unknown key \"" + key + "\"");
            else if (b.keys.size() < 2)
                b.keys.push_back(code);
        }
        for (const auto &input : parse_gamepad_inputs(split_list(touch.gamepad), "touch." + touch.name + "_gamepad", warnings)) {
            if (input.button >= 0)
                b.gamepad_buttons.push_back(input.button);
            else
                b.gamepad_axes.push_back(input.axis);
        }
        bindings.push_back(std::move(b));
    }
}

std::vector<short> Touches::claimed_gamepad_buttons() const {
    std::vector<short> buttons;
    for (const auto &b : bindings)
        for (const int button : b.gamepad_buttons)
            if (button >= 0)
                buttons.push_back(static_cast<short>(button));
    return buttons;
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
                for (const int button : b.gamepad_buttons)
                    down = down || SDL_GetGamepadButton(pad, static_cast<SDL_GamepadButton>(button));
                for (const int axis : b.gamepad_axes)
                    down = down || SDL_GetGamepadAxis(pad, static_cast<SDL_GamepadAxis>(axis)) > 16384;
            }
            // ids that stay the same while a finger is down, apart from the mouse's (0-127)
            if (down && b.target) {
                if (const auto point = b.target(emuenv))
                    held.push_back(at(point->first, point->second, static_cast<uint8_t>(100 + i % 16)));
            } else if (down)
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
