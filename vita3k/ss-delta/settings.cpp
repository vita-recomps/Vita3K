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

#include "settings.h"

#include <util/fs.h>
#include <util/string_utils.h>

#include <algorithm>
#include <charconv>
#include <fstream>
#include <map>
#include <sstream>

namespace ss_delta {

namespace {

constexpr const char *DEFAULT_INI = R"(; ss-delta settings. Lines starting with ; are comments; delete the ; to use a line.

[paths]
; The Vita file system with the installed game (ux0/app/PCSA00152) and firmware.
; Default: the fs folder next to ss-delta.exe.
;vita_fs = C:/Games/ss-delta/fs
; Where the recompiled modules (ssd_aot_<module>.dll) are. Default: next to ss-delta.exe.
;recompiled = C:/Games/ss-delta

[display]
; Draw the 3D at 960x544 like the menus and HUD (the game's own ResoDown 0). false: 720x408,
; upscaled, as on the Vita.
full_3d = true
; Internal resolution as a multiple of the Vita's 960x544 (1-8, fractions allowed).
scale = 1
; Window size as a multiple of 960x544 (1-4).
window_scale = 1
; Start in fullscreen. F11 or Alt+Enter switches while playing. F12 saves a screenshot to
; ss-delta/screenshots/ next to ss-delta.exe.
fullscreen = false
vsync = true
; How the picture is scaled to the window: Bilinear, Nearest, Bicubic, FXAA, FSR.
filter = Bilinear

[audio]
; 0-100.
volume = 100

[game]
; Unlock the add-ons (raiments, hoods, additional and otherworldly pacts). Their content is in the
; game's own data since update 1.30; PSN, which recorded the purchases, is closed. The save's
; system.bin is changed once (a copy goes to ss-delta/save-backups/).
dlc = true

[recompiled]
; Run the recompiled modules natively (false: everything on the JIT, for comparison).
enabled = true
; Keep the JIT (dynarmic) for any guest code that has no native code. Off: the game runs on
; native code alone, and stops with "no native code" in ss-delta/vita3k.log if it ever needs
; code that was not recompiled; turn this on then.
jit = false

[debug]
; Log detail in ss-delta/vita3k.log: trace, debug, info, warn, error, critical, off.
log_level = info
; The Vulkan validation layer (needs the Vulkan SDK; slow).
validation_layer = false

[keyboard]
; Vita button = key(s), up to two, separated by commas. Key names: A-Z, 0-9, Up, Down, Left,
; Right, Return, Space, Tab, Backspace, Escape, Left Shift, Right Shift, Left Ctrl, Right Ctrl,
; Left Alt, Right Alt, F1-F12, Keypad 0-9, or the W3C key codes (Comma, Period, Slash, Semicolon,
; Quote, BracketLeft, Minus, Equal ...). The mouse is the touchscreen: left button front, right
; button rear.
cross = X
circle = C
square = Z
triangle = V
l = Q
r = E
start = Return
select = Right Shift
up = Up
down = Down
left = Left
right = Right
analog_up = W
analog_down = S
analog_left = A
analog_right = D
right_analog_up = I
right_analog_down = K
right_analog_left = J
right_analog_right = L

[gamepad]
; Vita button = one gamepad button: south, east, west, north (the face buttons; on an Xbox pad
; A, B, X, Y), leftshoulder, rightshoulder, start, back, guide, dpup, dpdown, dpleft, dpright,
; leftstick, rightstick. The gamepad's sticks are the Vita's sticks.
cross = south
circle = east
square = west
triangle = north
l = leftshoulder
r = rightshoulder
start = start
select = back
up = dpup
down = dpdown
left = dpleft
right = dpright

[touch]
; Keys and gamepad buttons that hold a finger on the touchscreen for as long as they are held.
; Each touch is <name> = x, y (a point on the 960x544 screen), with <name>_key (up to two keys,
; as in [keyboard]) and <name>_gamepad (one button as in [gamepad], or lefttrigger or
; righttrigger). The mouse is the touchscreen as well.
; Black Rite: touch and hold the Black Rite icon, lower left during a quest.
black_rite = 56, 494
black_rite_key = F
black_rite_gamepad = lefttrigger

[boot_config]
; Engine settings passed to the game's boot_config.ini (Key = value, case as in that file), for
; experiments; they win over the settings above. The installed file is not changed.
;FPS = 30
)";

std::string trim(const std::string &s) {
    const auto b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos)
        return {};
    const auto e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

} // namespace

Settings load_settings(const fs::path &dir) {
    Settings out;
    out.vita_fs = dir / "fs" / "";
    out.recompiled = dir;

    const fs::path path = dir / "ss-delta.ini";
    if (!fs::exists(path)) {
        fs::ofstream file(path);
        file << DEFAULT_INI;
    }

    std::map<std::string, std::string> values; // "section.key" -> value
    {
        fs::ifstream file(path);
        std::string line, section;
        while (std::getline(file, line)) {
            line = trim(line);
            if (line.empty() || line[0] == ';' || line[0] == '#')
                continue;
            if (line.front() == '[' && line.back() == ']') {
                section = string_utils::tolower(trim(line.substr(1, line.size() - 2)));
                continue;
            }
            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                out.warnings.push_back(line);
                continue;
            }
            // the game's own keys are case-sensitive: kept as written
            if (section == "boot_config")
                out.boot_config[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
            else
                values[section + "." + string_utils::tolower(trim(line.substr(0, eq)))] = trim(line.substr(eq + 1));
        }
    }

    const auto get = [&](const char *key) -> const std::string * {
        const auto it = values.find(key);
        return it == values.end() || it->second.empty() ? nullptr : &it->second;
    };
    const auto get_bool = [&](const char *key, bool &dst) {
        if (const auto v = get(key)) {
            const auto s = string_utils::tolower(*v);
            if (s == "true" || s == "1" || s == "yes" || s == "on")
                dst = true;
            else if (s == "false" || s == "0" || s == "no" || s == "off")
                dst = false;
            else
                out.warnings.push_back(std::string(key) + " = " + *v);
        }
    };
    const auto get_number = [&](const char *key, auto &dst, auto lo, auto hi) {
        if (const auto v = get(key)) {
            std::remove_reference_t<decltype(dst)> x{};
            std::istringstream in(*v);
            if (in >> x && x >= lo && x <= hi)
                dst = x;
            else
                out.warnings.push_back(std::string(key) + " = " + *v);
        }
    };

    if (const auto v = get("paths.vita_fs"))
        out.vita_fs = fs_utils::utf8_to_path(*v) / "";
    if (const auto v = get("paths.recompiled"))
        out.recompiled = fs_utils::utf8_to_path(*v);
    get_bool("display.full_3d", out.full_3d);
    get_number("display.scale", out.scale, 1.0f, 8.0f);
    get_number("display.window_scale", out.window_scale, 1, 4);
    get_bool("display.fullscreen", out.fullscreen);
    get_bool("display.vsync", out.vsync);
    if (const auto v = get("display.filter")) {
        static const std::vector<std::string> filters{ "Bilinear", "Nearest", "Bicubic", "FXAA", "FSR" };
        const auto it = std::find_if(filters.begin(), filters.end(), [&](const std::string &f) {
            return string_utils::tolower(f) == string_utils::tolower(*v);
        });
        if (it != filters.end())
            out.filter = *it;
        else
            out.warnings.push_back("display.filter = " + *v);
    }
    get_number("audio.volume", out.volume, 0, 100);
    get_bool("game.dlc", out.dlc);
    get_bool("recompiled.enabled", out.use_recompiled);
    get_bool("recompiled.jit", out.jit);
    if (const auto v = get("debug.log_level")) {
        static const std::vector<std::string> levels{ "trace", "debug", "info", "warn", "error", "critical", "off" };
        const auto it = std::find(levels.begin(), levels.end(), string_utils::tolower(*v));
        if (it != levels.end())
            out.log_level = static_cast<int>(it - levels.begin());
        else
            out.warnings.push_back("debug.log_level = " + *v);
    }
    get_bool("debug.validation_layer", out.validation_layer);
    get_number("debug.frame_rate", out.frame_rate, 30, 60);
    get_number("debug.frame_rate_delay", out.frame_rate_delay, 0, 3600);
    std::map<std::string, Settings::TouchBinding> touches;
    for (const auto &[key, value] : values) {
        if (key.starts_with("keyboard."))
            out.keyboard[key.substr(9)] = value;
        else if (key.starts_with("gamepad."))
            out.gamepad[key.substr(8)] = value;
        else if (key.starts_with("touch.")) {
            const std::string name = key.substr(6);
            if (name.ends_with("_key"))
                touches[name.substr(0, name.size() - 4)].keys = value;
            else if (name.ends_with("_gamepad"))
                touches[name.substr(0, name.size() - 8)].gamepad = value;
            else {
                auto &touch = touches[name];
                char comma = 0;
                std::istringstream in(value);
                if (!(in >> touch.x >> comma >> touch.y) || comma != ',' || touch.x < 0 || touch.x >= 960 || touch.y < 0 || touch.y >= 544) {
                    out.warnings.push_back(key + " = " + value + " (x, y within 960x544)");
                    touch.x = touch.y = -1;
                }
            }
        }
    }
    for (auto &[name, touch] : touches) {
        if (touch.x < 0)
            continue;
        if (!values.contains("touch." + name)) {
            out.warnings.push_back("touch." + name + "_key/_gamepad without touch." + name + " = x, y");
            continue;
        }
        touch.name = name;
        out.touches.push_back(touch);
    }
    // an ss-delta.ini from before [touch] existed: the default's Black Rite binding
    if (std::none_of(values.begin(), values.end(), [](const auto &kv) { return kv.first.starts_with("touch."); }))
        out.touches.push_back({ "black_rite", 56, 494, "F", "lefttrigger" });
    return out;
}

} // namespace ss_delta
