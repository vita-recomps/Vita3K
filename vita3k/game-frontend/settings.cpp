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

#include <fmt/format.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>

namespace game_frontend {

namespace {

std::string trim(const std::string &s) {
    const auto b = s.find_first_not_of(" \t\r");
    if (b == std::string::npos)
        return {};
    const auto e = s.find_last_not_of(" \t\r");
    return s.substr(b, e - b + 1);
}

// "a\nb" -> "; a\n; b\n"
std::string comment_lines(const std::string &help) {
    if (help.empty())
        return {};
    std::string out;
    std::istringstream in(help);
    for (std::string line; std::getline(in, line);)
        out += line.empty() ? ";\n" : "; " + line + "\n";
    return out;
}

// the value a setting has when the file doesn't set it
std::string effective_default(const SettingSpec &spec) {
    return spec.commented ? std::string() : spec.value;
}

// `text` checked against `spec`: the value to keep (choices in their listed spelling), or nothing
std::optional<std::string> check(const SettingSpec &spec, const std::string &text) {
    using Kind = SettingSpec::Kind;
    switch (spec.kind) {
    case Kind::Bool: {
        const auto s = string_utils::tolower(text);
        if (s == "true" || s == "1" || s == "yes" || s == "on")
            return "true";
        if (s == "false" || s == "0" || s == "no" || s == "off")
            return "false";
        return std::nullopt;
    }
    case Kind::Int: {
        long long x = 0;
        std::istringstream in(text);
        if (in >> x && in.eof() && x >= spec.min && x <= spec.max)
            return std::to_string(x);
        return std::nullopt;
    }
    case Kind::Float: {
        double x = 0;
        std::istringstream in(text);
        if (in >> x && x >= spec.min && x <= spec.max)
            return text;
        return std::nullopt;
    }
    case Kind::Choice: {
        const auto it = std::find_if(spec.choices.begin(), spec.choices.end(), [&](const std::string &c) {
            return string_utils::tolower(c) == string_utils::tolower(text);
        });
        if (it == spec.choices.end())
            return std::nullopt;
        return *it;
    }
    case Kind::Path:
    case Kind::Text:
        return text;
    }
    return std::nullopt;
}

} // namespace

const std::string &Values::get(const std::string &key) const {
    static const std::string none;
    const auto it = values.find(key);
    return it == values.end() ? none : it->second;
}

bool Values::get_bool(const std::string &key) const {
    return get(key) == "true";
}

int Values::get_int(const std::string &key) const {
    const auto &v = get(key);
    return v.empty() ? 0 : std::atoi(v.c_str());
}

double Values::get_float(const std::string &key) const {
    const auto &v = get(key);
    return v.empty() ? 0.0 : std::atof(v.c_str());
}

fs::path Values::get_path(const std::string &key) const {
    const auto &v = get(key);
    return v.empty() ? fs::path() : fs_utils::utf8_to_path(v);
}

const std::map<std::string, std::string> &Values::section(const std::string &name) const {
    static const std::map<std::string, std::string> none;
    const auto it = free_sections.find(name);
    return it == free_sections.end() ? none : it->second;
}

bool Values::has_section(const std::string &name) const {
    return sections_in_file.contains(name);
}

std::string default_ini(const IniLayout &layout) {
    std::string out = comment_lines(layout.header);
    for (const auto &section : layout.sections) {
        out += "\n[" + section.name + "]\n" + comment_lines(section.help);
        for (const auto &spec : section.settings)
            out += comment_lines(spec.help) + (spec.commented ? ";" : "") + spec.key + " = " + spec.value + "\n";
        out += section.body;
    }
    return out;
}

fs::path settings_file(const fs::path &dir, const std::string &id, int player) {
    return player >= 2 ? dir / fmt::format("{}-player{}.ini", id, player) : dir / (id + ".ini");
}

bool save_setting(const fs::path &file, const std::string &section, const std::string &key, const std::string &value, std::string &error) {
    // read and written as bytes, so the file keeps its line endings (CRLF or LF)
    std::vector<std::string> lines;
    bool crlf = false;
    {
        fs::ifstream in(file, std::ios::binary);
        for (std::string line; std::getline(in, line);) {
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
                crlf = true;
            }
            lines.push_back(line);
        }
    }
    const auto lower = [](std::string s) { return string_utils::tolower(s); };
    // the key of a "key = value" or ";key = value" line, lower case, and whether it is commented
    const auto key_of = [&](const std::string &line, bool &commented) -> std::string {
        std::string t = trim(line);
        commented = t.starts_with(";");
        if (commented)
            t = trim(t.substr(1));
        const auto eq = t.find('=');
        return eq == std::string::npos ? std::string() : lower(trim(t.substr(0, eq)));
    };
    const std::string wanted_section = lower(section), wanted_key = lower(key);
    std::string current;
    std::optional<size_t> header, set_line, commented_line;
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string t = trim(lines[i]);
        if (t.starts_with("[") && t.ends_with("]")) {
            current = lower(trim(t.substr(1, t.size() - 2)));
            if (current == wanted_section && !header)
                header = i;
            continue;
        }
        if (current != wanted_section)
            continue;
        bool commented = false;
        if (key_of(lines[i], commented) == wanted_key) {
            if (!commented && !set_line)
                set_line = i;
            else if (commented && !commented_line)
                commented_line = i;
        }
    }
    const std::string new_line = key + " = " + value;
    if (set_line)
        lines[*set_line] = new_line;
    else if (commented_line)
        lines[*commented_line] = new_line;
    else if (header)
        lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(*header + 1), new_line);
    else {
        lines.push_back("");
        lines.push_back("[" + section + "]");
        lines.push_back(new_line);
    }
    // written to a new file first, which then replaces the old one
    const fs::path tmp = fs::path(file).concat(".new");
    {
        fs::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        for (const auto &line : lines)
            out << line << (crlf ? "\r\n" : "\n");
        if (!out) {
            error = "cannot write " + fs_utils::path_to_utf8(tmp);
            return false;
        }
    }
    boost::system::error_code ec;
    fs::rename(tmp, file, ec);
    if (ec) {
        error = fmt::format("cannot replace {}: {}", fs_utils::path_to_utf8(file), ec.message());
        return false;
    }
    return true;
}

Settings load_settings(const fs::path &dir, const std::string &id, const IniLayout &layout,
    const std::vector<TouchBinding> &default_touches, int player) {
    Settings out;
    out.vita_fs = dir / "fs" / "";
    out.recompiled = dir;

    const fs::path path = dir / (id + ".ini");
    if (!fs::exists(path)) {
        fs::ofstream file(path);
        file << default_ini(layout);
    }
    std::vector<fs::path> paths{ path };
    if (player >= 2 && fs::exists(dir / fmt::format("{}-player{}.ini", id, player)))
        paths.push_back(dir / fmt::format("{}-player{}.ini", id, player));

    std::map<std::string, const IniSection *> sections;
    for (const auto &section : layout.sections)
        sections[section.name] = &section;

    std::map<std::string, std::string> raw; // "section.key" -> text as written
    auto &values = out.values;
    for (const auto &file_path : paths) {
        fs::ifstream file(file_path);
        std::string line, section;
        bool first_line = true;
        while (std::getline(file, line)) {
            // a UTF-8 byte order mark, as Notepad and PowerShell may write
            if (first_line && line.starts_with("\xEF\xBB\xBF"))
                line.erase(0, 3);
            first_line = false;
            line = trim(line);
            if (line.empty() || line[0] == ';' || line[0] == '#')
                continue;
            if (line.front() == '[' && line.back() == ']') {
                section = string_utils::tolower(trim(line.substr(1, line.size() - 2)));
                values.sections_in_file.insert(section);
                continue;
            }
            const auto eq = line.find('=');
            if (eq == std::string::npos) {
                out.warnings.push_back(line);
                continue;
            }
            const auto it = sections.find(section);
            const bool keep_case = it != sections.end() && it->second->keep_case;
            const std::string key = keep_case ? trim(line.substr(0, eq)) : string_utils::tolower(trim(line.substr(0, eq)));
            const std::string value = trim(line.substr(eq + 1));
            if (it != sections.end() && it->second->free_keys)
                values.free_sections[section][key] = value;
            else
                raw[section + "." + key] = value;
        }
    }

    // the declared settings: the file's value if it is valid, otherwise the default
    for (const auto &section : layout.sections) {
        for (const auto &spec : section.settings) {
            const std::string key = section.name + "." + spec.key;
            std::string value = effective_default(spec);
            if (const auto it = raw.find(key); it != raw.end() && !it->second.empty()) {
                if (const auto checked = check(spec, it->second))
                    value = *checked;
                else
                    out.warnings.push_back(key + " = " + it->second);
            }
            values.values[key] = value;
        }
    }

    // the shared settings
    if (const auto v = values.get_path("paths.vita_fs"); !v.empty())
        out.vita_fs = v / "";
    if (const auto v = values.get_path("paths.recompiled"); !v.empty())
        out.recompiled = v;
    const auto declared = [&](const char *key) { return values.values.contains(key); };
    if (declared("display.scale"))
        out.scale = static_cast<float>(values.get_float("display.scale"));
    if (declared("display.window_scale"))
        out.window_scale = values.get_int("display.window_scale");
    if (declared("display.fullscreen"))
        out.fullscreen = values.get_bool("display.fullscreen");
    if (declared("display.vsync"))
        out.vsync = values.get_bool("display.vsync");
    if (declared("display.filter"))
        out.filter = values.get("display.filter");
    if (declared("audio.volume"))
        out.volume = values.get_int("audio.volume");
    if (declared("recompiled.enabled"))
        out.use_recompiled = values.get_bool("recompiled.enabled");
    if (declared("recompiled.jit"))
        out.jit = values.get_bool("recompiled.jit");
    if (declared("debug.log_level")) {
        static const std::vector<std::string> levels{ "trace", "debug", "info", "warn", "error", "critical", "off" };
        out.log_level = static_cast<int>(std::find(levels.begin(), levels.end(), values.get("debug.log_level")) - levels.begin());
    }
    if (declared("debug.validation_layer"))
        out.validation_layer = values.get_bool("debug.validation_layer");
    out.keyboard = values.section("keyboard");
    out.gamepad = values.section("gamepad");

    std::map<std::string, TouchBinding> touches;
    const auto &touch_values = values.section("touch");
    for (const auto &[name, value] : touch_values) {
        if (name.ends_with("_key"))
            touches[name.substr(0, name.size() - 4)].keys = value;
        else if (name.ends_with("_gamepad"))
            touches[name.substr(0, name.size() - 8)].gamepad = value;
        else {
            auto &touch = touches[name];
            char comma = 0;
            std::istringstream in(value);
            if (!(in >> touch.x >> comma >> touch.y) || comma != ',' || touch.x < 0 || touch.x >= 960 || touch.y < 0 || touch.y >= 544) {
                out.warnings.push_back("touch." + name + " = " + value + " (x, y within 960x544)");
                touch.x = touch.y = -1;
            }
        }
    }
    for (auto &[name, touch] : touches) {
        if (touch.x < 0)
            continue;
        if (!touch_values.contains(name)) {
            out.warnings.push_back("touch." + name + "_key/_gamepad without touch." + name + " = x, y");
            continue;
        }
        touch.name = name;
        out.touches.push_back(touch);
    }
    // a file from before [touch] existed: the game's default bindings
    if (!values.has_section("touch") && touch_values.empty())
        out.touches = default_touches;
    return out;
}

namespace layout {

using Kind = SettingSpec::Kind;

IniSection paths(const std::string &title_id, const std::string &id, const std::string &exe) {
    return { "paths", "",
        {
            { "vita_fs", Kind::Path, fmt::format("C:/Games/{}/fs", id), 0, 0, {},
                fmt::format("The Vita file system with the installed game (ux0/app/{}) and firmware.\n"
                            "Default: the fs folder next to {}.",
                    title_id, exe),
                true },
            { "recompiled", Kind::Path, fmt::format("C:/Games/{}", id), 0, 0, {},
                fmt::format("Where the recompiled modules (ssd_aot_<module>.dll) are. Default: next to {}.", exe), true },
        } };
}

SettingSpec scale() {
    SettingSpec s{ "scale", Kind::Float, "1", 1, 8, {}, "Internal resolution as a multiple of the Vita's 960x544 (1-8, fractions allowed)." };
    s.title = "Resolution";
    return s;
}

SettingSpec window_scale() {
    SettingSpec s{ "window_scale", Kind::Int, "1", 1, 4, {}, "Window size as a multiple of 960x544 (1-4)." };
    s.live = true;
    return s;
}

SettingSpec fullscreen(const std::string &id, const std::string &exe) {
    SettingSpec s{ "fullscreen", Kind::Bool, "false", 0, 0, {},
        fmt::format("Start in fullscreen. F11 or Alt+Enter switches while playing. F12 saves a screenshot to\n"
                    "{}/screenshots/ next to {}.",
            id, exe) };
    s.live = true;
    return s;
}

SettingSpec vsync() {
    SettingSpec s{ "vsync", Kind::Bool, "true", 0, 0, {}, "Wait for the display's refresh (no tearing)." };
    s.live = true;
    s.title = "VSync";
    return s;
}

SettingSpec filter() {
    SettingSpec s{ "filter", Kind::Choice, "Bilinear", 0, 0, { "Bilinear", "Nearest", "Bicubic", "FXAA", "FSR" },
        "How the picture is scaled to the window: Bilinear, Nearest, Bicubic, FXAA, FSR." };
    s.live = true;
    return s;
}

SettingSpec mipmaps(bool on_by_default) {
    SettingSpec s{ "mipmaps", Kind::Bool, on_by_default ? "true" : "false", 0, 0, {},
        "Smooth textures that shimmer in motion (the ground, walls): build mip levels for the repeating\n"
        "textures the game ships without them (false: as on the Vita). Read when the game starts." };
    s.title = "Smooth textures (mipmaps)";
    return s;
}

IniSection audio() {
    SettingSpec volume{ "volume", Kind::Int, "100", 0, 100, {}, "0-100." };
    volume.live = true;
    return { "audio", "", { volume } };
}

IniSection recompiled(const std::string &id) {
    return { "recompiled", "",
        {
            { "enabled", Kind::Bool, "true", 0, 0, {}, "Run the recompiled modules natively (false: everything on the JIT, for comparison)." },
            { "jit", Kind::Bool, "false", 0, 0, {},
                fmt::format("Keep the JIT (dynarmic) for any guest code that has no native code. Off: the game runs on\n"
                            "native code alone, and stops with \"no native code\" in {}/vita3k.log if it ever needs\n"
                            "code that was not recompiled; turn this on then.",
                    id) },
        } };
}

IniSection debug(const std::string &id) {
    return { "debug", "",
        {
            { "log_level", Kind::Choice, "info", 0, 0, { "trace", "debug", "info", "warn", "error", "critical", "off" },
                fmt::format("Log detail in {}/vita3k.log: trace, debug, info, warn, error, critical, off.", id) },
            { "validation_layer", Kind::Bool, "false", 0, 0, {}, "The Vulkan validation layer (needs the Vulkan SDK; slow)." },
        } };
}

IniSection keyboard(const std::string &extra_help) {
    IniSection s{ "keyboard",
        "Vita button = key(s), up to two, separated by commas. Key names: A-Z, 0-9, Up, Down, Left,\n"
        "Right, Return, Space, Tab, Backspace, Escape, Left Shift, Right Shift, Left Ctrl, Right Ctrl,\n"
        "Left Alt, Right Alt, F1-F12, Keypad 0-9, or the W3C key codes (Comma, Period, Slash, Semicolon,\n"
        "Quote, BracketLeft, Minus, Equal ...). The mouse is the touchscreen: left button front, right\n"
        "button rear."
            + (extra_help.empty() ? "" : "\n" + extra_help) };
    s.free_keys = true;
    s.body = "cross = X\ncircle = C\nsquare = Z\ntriangle = V\nl = Q\nr = E\nstart = Return\nselect = Right Shift\n"
             "up = Up\ndown = Down\nleft = Left\nright = Right\n"
             "analog_up = W\nanalog_down = S\nanalog_left = A\nanalog_right = D\n"
             "right_analog_up = I\nright_analog_down = K\nright_analog_left = J\nright_analog_right = L\n";
    return s;
}

IniSection gamepad() {
    IniSection s{ "gamepad",
        "Vita button = one gamepad button: south, east, west, north (the face buttons; on an Xbox pad\n"
        "A, B, X, Y), leftshoulder, rightshoulder, start, back, guide, dpup, dpdown, dpleft, dpright,\n"
        "leftstick, rightstick. The gamepad's sticks are the Vita's sticks." };
    s.free_keys = true;
    s.body = "cross = south\ncircle = east\nsquare = west\ntriangle = north\nl = leftshoulder\nr = rightshoulder\n"
             "start = start\nselect = back\nup = dpup\ndown = dpdown\nleft = dpleft\nright = dpright\n";
    return s;
}

IniSection touch(const std::string &body) {
    IniSection s{ "touch",
        "Keys and gamepad buttons that hold a finger on the touchscreen for as long as they are held.\n"
        "Each touch is <name> = x, y (a point on the 960x544 screen), with <name>_key (up to two keys,\n"
        "as in [keyboard]) and <name>_gamepad (one button as in [gamepad], or lefttrigger or\n"
        "righttrigger). The mouse is the touchscreen as well." };
    s.free_keys = true;
    s.body = body;
    return s;
}

} // namespace layout

} // namespace game_frontend
