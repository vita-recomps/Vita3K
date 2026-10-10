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
; Internal resolution as a multiple of the Vita's 960x544 (1-8, fractions allowed).
scale = 1
; Window size as a multiple of 960x544 (1-4).
window_scale = 1
; Start in fullscreen. F11 or Alt+Enter switches while playing.
fullscreen = false
vsync = true
; How the picture is scaled to the window: Bilinear, Nearest, Bicubic, FXAA, FSR.
filter = Bilinear

[audio]
; 0-100.
volume = 100

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
    for (const auto &[key, value] : values) {
        if (key.starts_with("keyboard."))
            out.keyboard[key.substr(9)] = value;
        else if (key.starts_with("gamepad."))
            out.gamepad[key.substr(8)] = value;
    }
    return out;
}

} // namespace ss_delta
