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
#include "input.h"

#include <util/fs.h>
#include <util/string_utils.h>

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>

namespace game_frontend {

#ifdef __ANDROID__
std::vector<std::string> android_driver_names(); // android.cpp
#endif

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
        for (const auto &spec : section.settings) {
#ifdef __ANDROID__
            if (spec.desktop_only)
                continue;
#endif
            out += comment_lines(spec.help) + (spec.commented ? ";" : "") + spec.key + " = " + spec.value + "\n";
        }
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

std::string settings_help(const IniLayout &layout) {
    std::string out;
    for (const auto &section : layout.sections) {
        if (section.free_keys)
            continue;
        for (const auto &spec : section.settings) {
            std::string line = "  " + section.name + "." + spec.key;
            std::string detail;
            switch (spec.kind) {
            case SettingSpec::Kind::Bool: detail = "true/false"; break;
            case SettingSpec::Kind::Int: detail = fmt::format("{}..{}", spec.min, spec.max); break;
            case SettingSpec::Kind::Float: detail = fmt::format("{}..{}", spec.min, spec.max); break;
            case SettingSpec::Kind::Choice:
                for (const auto &choice : spec.choices)
                    detail += (detail.empty() ? "" : "/") + choice;
                break;
            case SettingSpec::Kind::Path: detail = "a path"; break;
            case SettingSpec::Kind::Text: break;
            }
            const std::string value = effective_default(spec);
            if (!detail.empty())
                line += " (" + detail + ")";
            if (!value.empty())
                line += ", default " + value;
            out += line + "\n";
        }
    }
    return out;
}

Settings load_settings(const fs::path &dir, const std::string &id, const IniLayout &layout,
    const std::vector<TouchBinding> &default_touches, int player, const std::vector<std::string> &overrides) {
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

    // the command line's --set section.key=value, over the files
    for (const auto &text : overrides) {
        const auto eq = text.find('=');
        const std::string key = string_utils::tolower(trim(text.substr(0, eq)));
        const auto dot = key.find('.');
        const auto it = dot == std::string::npos ? sections.end() : sections.find(key.substr(0, dot));
        const bool declared = it != sections.end() && std::any_of(it->second->settings.begin(), it->second->settings.end(),
                                                         [&](const SettingSpec &spec) { return key == it->second->name + "." + spec.key; });
        if (eq == std::string::npos || !declared) {
            out.warnings.push_back("--set " + text + " (no such setting; --help lists them)");
            continue;
        }
        raw[key] = trim(text.substr(eq + 1));
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
    if (declared("display.fps_counter"))
        out.fps_counter = values.get("display.fps_counter");
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
    if (declared("debug.surface_sync")) {
        const std::array<std::string, 4> modes = { "exact", "targeted", "latent", "off" };
        const auto v = values.get("debug.surface_sync");
        for (int i = 0; i < static_cast<int>(modes.size()); i++)
            if (v == modes[i])
                out.surface_sync = i;
    }
    if (declared("debug.high_accuracy"))
        out.high_accuracy = values.get_bool("debug.high_accuracy");
    if (declared("debug.framebuffer_reads"))
        out.framebuffer_reads = values.get("debug.framebuffer_reads");
    if (declared("debug.shader_blend"))
        out.shader_blend = values.get("debug.shader_blend");
    if (declared("debug.net_trace"))
        out.net_trace = values.get_bool("debug.net_trace");
    if (declared("debug.gpu"))
        out.gpu = values.get_int("debug.gpu");
    if (declared("debug.hitch_log_ms"))
        out.hitch_log_ms = values.get_int("debug.hitch_log_ms");
    if (declared("debug.gpu_driver"))
        out.gpu_driver = values.get("debug.gpu_driver");
    out.keyboard = values.section("keyboard");
    out.gamepad = values.section("gamepad");

    // A touch the game binds by default starts from that binding, so a line the file lacks keeps the
    // default: a file from before [touch] existed gets only the line the Controls page changed (e.g.
    // black_rite_key), and the touch's spot and other inputs must survive the next start.
    std::map<std::string, TouchBinding> touches;
    const auto touch_entry = [&](const std::string &name) -> TouchBinding & {
        auto it = touches.find(name);
        if (it == touches.end()) {
            const auto def = std::find_if(default_touches.begin(), default_touches.end(), [&](const TouchBinding &t) { return t.name == name; });
            it = touches.emplace(name, def != default_touches.end() ? *def : TouchBinding{}).first;
        }
        return it->second;
    };
    const auto &touch_values = values.section("touch");
    for (const auto &[name, value] : touch_values) {
        if (name.ends_with("_key"))
            touch_entry(name.substr(0, name.size() - 4)).keys = value;
        else if (name.ends_with("_gamepad"))
            touch_entry(name.substr(0, name.size() - 8)).gamepad = value;
        else {
            auto &touch = touch_entry(name);
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
        // a default touch (its name already set) has its spot; any other needs its x, y line
        if (touch.name.empty() && !touch_values.contains(name)) {
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
#ifdef _WIN32
    const std::string example = fmt::format("C:/Games/{}", id), module = "ssd_aot_<module>.dll";
#elif defined(__APPLE__)
    const std::string example = fmt::format("/Users/you/Games/{}", id), module = "ssd_aot_<module>.dylib";
#else
    const std::string example = fmt::format("/home/deck/Games/{}", id), module = "ssd_aot_<module>.so";
#endif
    return { "paths", "",
        {
            { "vita_fs", Kind::Path, example + "/fs", 0, 0, {},
                fmt::format("The Vita file system with the installed game (ux0/app/{}) and firmware.\n"
                            "Default: the fs folder next to {}.",
                    title_id, exe),
                true },
            { "recompiled", Kind::Path, example, 0, 0, {},
                fmt::format("Where the recompiled modules ({}) are. Default: next to {}.", module, exe), true },
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
    s.desktop_only = true;
    return s;
}

SettingSpec fullscreen(const std::string &id, const std::string &exe) {
    SettingSpec s{ "fullscreen", Kind::Bool, "false", 0, 0, {},
        fmt::format("Start in fullscreen. F11 or Alt+Enter switches while playing. F12 saves a screenshot to\n"
                    "{}/screenshots/ next to {}.",
            id, exe) };
    s.live = true;
    s.desktop_only = true;
    return s;
}

SettingSpec vsync() {
    SettingSpec s{ "vsync", Kind::Bool, "true", 0, 0, {}, "Wait for the display's refresh (no tearing)." };
    s.live = true;
    s.title = "VSync";
    return s;
}

SettingSpec filter() {
#ifdef __ANDROID__
    const char *const target = "the screen";
#else
    const char *const target = "the window";
#endif
    SettingSpec s{ "filter", Kind::Choice, "Bilinear", 0, 0, { "Bilinear", "Nearest", "Bicubic", "FXAA", "FSR" },
        fmt::format("How the picture is scaled to {}: Bilinear, Nearest, Bicubic, FXAA, FSR.", target) };
    s.live = true;
    return s;
}

SettingSpec fps_counter() {
    SettingSpec s{ "fps_counter", Kind::Choice, "off", 0, 0, { "off", "fps", "frametime", "stats", "graph" },
        "A frame counter in the top left corner: off, fps (frames a second), frametime (and the\n"
        "milliseconds a frame), stats (and the lowest, average and highest of the last minute), graph\n"
        "(and a graph of them)." };
    s.live = true;
    s.title = "FPS counter";
    s.choice_labels = { "Off", "FPS", "FPS, frame time", "FPS, frame time, min/avg/max", "All, with a graph" };
    return s;
}

SettingSpec mipmaps(bool on_by_default) {
    SettingSpec s{ "mipmaps", Kind::Bool, on_by_default ? "true" : "false", 0, 0, {},
        "Smooth textures that shimmer in motion (the ground, walls): build mip levels for the repeating\n"
        "textures the game ships without them (Off: as on the Vita). Read when the game starts." };
    s.title = "Smooth textures (mipmaps)";
    return s;
}

SettingSpec repatch() {
    SettingSpec s{ "repatch", Kind::Bool, "true", 0, 0, {},
        "Mods: read the files in the Vita file system's ux0/rePatch/<title ID>/ in place of the game's own,\n"
        "as the rePatch plugin does on a Vita (an undub, for instance). Install one with the setup, or\n"
        "--install-content <its folder or .zip>; the game's own files stay as they are. false: the game as\n"
        "installed. Read when the game starts." };
    s.title = "Mods (rePatch)";
    return s;
}

IniSection audio() {
    SettingSpec volume{ "volume", Kind::Int, "100", 0, 100, {}, "0-100." };
    volume.live = true;
    return { "audio", "", { volume } };
}

IniSection recompiled(const std::string &id) {
    IniSection s{ "recompiled", "",
        {
            { "enabled", Kind::Bool, "true", 0, 0, {}, "Run the recompiled modules natively (Off: everything on the JIT, for comparison)." },
            { "jit", Kind::Bool, "false", 0, 0, {},
                fmt::format("Keep the JIT (dynarmic) for any guest code that has no native code. Off: the game runs on\n"
                            "native code alone, and stops with \"no native code\" in {}/vita3k.log if it ever needs\n"
                            "code that was not recompiled; turn this on then.",
                    id) },
        } };
    s.settings[1].title = "Keep the JIT";
    return s;
}

IniSection debug(const std::string &id) {
    IniSection s{ "debug", "",
        {
            { "log_level", Kind::Choice, "info", 0, 0, { "trace", "debug", "info", "warn", "error", "critical", "off" },
                fmt::format("Log detail in {}/vita3k.log: trace, debug, info, warn, error, critical, off.", id) },
            { "validation_layer", Kind::Bool, "false", 0, 0, {}, "The Vulkan validation layer (needs the Vulkan SDK; slow)." },
            { "surface_sync", Kind::Choice, "exact", 0, 0, { "exact", "targeted", "latent", "off" },
                "Vita3K's surface sync: what the game renders and then reads on the CPU is copied back.\n"
                "exact: every scene waits for the GPU. targeted: only scenes the CPU reads back wait.\n"
                "latent: none waits, the CPU reads a frame-old copy. off: no copy (effects may break)." },
            { "high_accuracy", Kind::Bool, "true", 0, 0, {},
                "Vita3K's high-accuracy rendering (steady smoke). Off is faster but can flicker." },
            { "framebuffer_reads", Kind::Choice, "auto", 0, 0, { "auto", "subpass", "subpass_storage", "copy" },
                "How smoke, fire and fog read the picture on a graphics card without shader interlock (the log's\n"
                "\"Framebuffer reads:\" line says \"no, no\"): auto (copy), subpass (as in 0.5.1), subpass_storage, copy." },
            { "shader_blend", Kind::Choice, "auto", 0, 0, { "auto", "on", "off" },
                "Smoke and fire whose shaders read the picture only to blend: blended by the graphics card's\n"
                "blender instead. auto: on cards with neither shader interlock nor raster-order access. on, off." },
            { "net_trace", Kind::Bool, "false", 0, 0, {},
                fmt::format("Every Ad Hoc Play packet sent and received in {}/vita3k.log, for a connection problem\n"
                            "(the log grows quickly: leave it off otherwise).", id) },
            { "gpu", Kind::Int, "0", 0, 16, {},
                fmt::format("The graphics card: 0 picks one (a dedicated card first); N, the Nth of the\n"
                            "\"Vulkan devices\" {}/vita3k.log lists (for a laptop that starts on the wrong one).", id) },
            { "hitch_log_ms", Kind::Int, "100", 0, 1000, {},
                fmt::format("A game frame slower than this many milliseconds gets a \"Hitch\" line in {}/vita3k.log,\n"
                            "with the shaders built meanwhile (for a stutter report). 0: off.", id) },
#ifdef __ANDROID__
            { "gpu_driver", Kind::Text, "auto", 0, 0, {},
                "The Vulkan driver: auto (the driver package installed last from drivers/, such as the Turnip\n"
                "the game offers to download on Adreno GPUs; else the system's), system, or the name of a\n"
                "package (<name> for drivers/<name>.adpkg.zip). One that fails to load falls back to the system's." },
#endif
        } };
    for (auto &spec : s.settings) {
        // the validation layer comes with a PC's Vulkan SDK
        if (spec.key == "validation_layer")
            spec.desktop_only = true;
        if (spec.key == "hitch_log_ms")
            spec.title = "Log slow frames (ms)";
#ifdef __ANDROID__
        // the settings menu offers the packages on the device (a Text setting can only be changed in the
        // settings file, which a player can't reach on Android)
        if (spec.key == "gpu_driver") {
            spec.title = "GPU driver";
            spec.kind = Kind::Choice;
            spec.choices = { "auto", "system" };
            spec.choice_labels = { "Auto (newest package)", "Device's own" };
            for (const auto &name : android_driver_names()) {
                spec.choices.push_back(name);
                spec.choice_labels.push_back(name);
            }
        }
#endif
    }
    return s;
}

IniSection keyboard(const std::string &extra_help) {
    IniSection s{ "keyboard",
        "Vita button = key(s), up to two, separated by commas. Key names: A-Z, 0-9, Up, Down, Left,\n"
        "Right, Return, Space, Tab, Backspace, Escape, Left Shift, Right Shift, Left Ctrl, Right Ctrl,\n"
        "Left Alt, Right Alt, F1-F12, Keypad 0-9, or the W3C key codes (Comma, Period, Slash, Semicolon,\n"
        "Quote, BracketLeft, Minus, Equal ...). Also l2 and r2 (U and O unless set), a PS TV's L2/R2.\n"
        "Mouse buttons: MouseX1, MouseX2 (the side buttons), and MouseLeft, MouseRight, which press their\n"
        "button only while the mouse camera has the cursor; otherwise the mouse is the touchscreen (left\n"
        "button front, right button rear). The settings menu's Controls page changes them too."
            + (extra_help.empty() ? "" : "\n" + extra_help) };
    s.free_keys = true;
    s.body = default_keyboard_lines();
    return s;
}

IniSection gamepad() {
    IniSection s{ "gamepad",
        "Vita button = up to two gamepad buttons, comma separated: south, east, west, north (the face\n"
        "buttons; on an Xbox pad A, B, X, Y), leftshoulder, rightshoulder, start, back, guide, dpup,\n"
        "dpdown, dpleft, dpright, leftstick, rightstick, or a trigger, lefttrigger or righttrigger (which\n"
        "stays a PS TV's L2 or R2 as well), e.g. up = dpup, righttrigger; nothing after the = unbinds it.\n"
        "The gamepad's sticks are the Vita's sticks. The settings menu's Controls page changes them too." };
    s.free_keys = true;
    s.body = default_gamepad_lines();
    return s;
}

IniSection mouse() {
    IniSection s{ "mouse",
        "The mouse camera. Middle-click or press capture_key to capture the cursor: it hides, and moving\n"
        "the mouse turns the camera (the right stick). The same again or Esc releases it, and the mouse\n"
        "is the touchscreen again (left button front, right button rear). The settings menu, a text\n"
        "dialog and switching to another window release it until they are done.",
        {
            { "camera", Kind::Bool, "true", 0, 0, {}, "Off: the mouse is only the touchscreen, the cursor is never captured." },
            { "capture_key", Kind::Text, "Tab", 0, 0, {}, "Up to two keys or mouse buttons that capture and release the cursor. Game bindings take priority." },
            { "sensitivity", Kind::Float, "1", 0.2, 4, {},
                "How far the camera turns for a mouse motion (0.2-4). 1: a full turn for 7200 counts (23 cm at\n"
                "800 dpi). The camera turns at most as fast as with the stick fully pushed, so a flick faster\n"
                "than that turns less than it asks." },
            { "invert_y", Kind::Bool, "false", 0, 0, {}, "Moving the mouse up turns the camera down." },
            { "hide_cursor", Kind::Bool, "true", 0, 0, {},
                "Hide the cursor when the mouse has been still for 3 seconds, and at once when a key or a gamepad\n"
                "is used; moving the mouse or clicking shows it again. Off: the cursor always shows (except\n"
                "while the mouse camera has it)." },
        } };
    s.settings[0].title = "Mouse camera";
    s.settings[3].title = "Invert vertical";
    s.settings[4].title = "Hide idle cursor";
    for (auto &spec : s.settings) {
        spec.live = true;
        spec.desktop_only = true;
    }
    return s;
}

IniSection touch(const std::string &body) {
    IniSection s{ "touch",
        "Keys and gamepad buttons that hold a finger on the touchscreen for as long as they are held.\n"
        "Each touch is <name> = x, y (a point on the 960x544 screen), with <name>_key (up to two keys,\n"
        "as in [keyboard]) and <name>_gamepad (up to two buttons or triggers, as in [gamepad]). The\n"
        "mouse is the touchscreen as well. The settings menu's Controls page changes the keys and\n"
        "buttons." };
    s.free_keys = true;
    s.body = body;
    return s;
}

IniSection on_screen_controls() {
    IniSection s{ "controls", "",
        {
            { "on_screen", Kind::Bool, "true", 0, 0, {},
                "Buttons and sticks on the touchscreen; a finger anywhere else touches the game itself. They hide\n"
                "while a gamepad is used and come back when it is disconnected. Off: only the menu button at the\n"
                "top stays, and they show while the settings menu is open." },
            { "on_screen_size", Kind::Int, "100", 50, 150, {}, "Their size, in percent (50-150)." },
            { "on_screen_opacity", Kind::Int, "60", 10, 100, {}, "How opaque they are, in percent (10-100)." },
        } };
    s.settings[0].title = "On-screen controls";
    s.settings[1].title = "Size";
    s.settings[2].title = "Opacity";
    for (auto &spec : s.settings)
        spec.live = true;
    return s;
}

} // namespace layout

} // namespace game_frontend
