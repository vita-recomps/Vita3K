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

// The game's settings file (<id>.ini next to the executable). Its settings are declared as data
// (IniLayout): the default file is written from the declarations, and the file is read and checked
// against them, so a settings screen can be built from the same list without code for each game.

#include <util/fs.h>

#include <map>
#include <set>
#include <string>
#include <vector>

namespace game_frontend {

struct SettingSpec {
    enum class Kind {
        Bool, // true/false (also 1/0, yes/no, on/off)
        Int, // a whole number in [min, max]
        Float, // a number in [min, max]
        Choice, // one of choices, matched without case, kept as listed
        Path, // a folder or file
        Text,
    };
    std::string key; // lower case
    Kind kind = Kind::Text;
    std::string value; // the default, as written in the file; empty: unset
    double min = 0, max = 0;
    std::vector<std::string> choices;
    std::string help; // comment lines above the key in the default file, without "; "
    bool commented = false; // in the default file as ";key = value" (an example, not set)
    // the settings menu changes it while the game runs (frontend.cpp applies the shared ones, a game's
    // hooks read its own each tick); otherwise a change takes effect at the next start
    bool live = false;
    std::string title; // the settings menu's name for it; empty: from the key ("window_scale" -> "Window scale")
    // a Choice's names in the settings menu, one per choice (empty: the choices as written)
    std::vector<std::string> choice_labels;
    // for a PC only (a window, a keyboard, several copies on one machine): Android's settings file and
    // settings menu leave it out; its default still applies there
    bool desktop_only = false;
    // a Text the settings menu edits: typed on a keyboard (Android's on-screen one too), or with a
    // gamepad, Up/Down changing the character at the cursor (text_chars, in that order)
    bool text_entry = false;
    std::string text_chars;
};

struct IniSection {
    std::string name; // [name], lower case
    std::string help; // comment lines under the header, without "; "
    std::vector<SettingSpec> settings;
    // keyboard, gamepad, touch, boot_config: any key; `body` holds the default file's lines
    bool free_keys = false;
    bool keep_case = false; // keys as written (the game's own keys), otherwise lower case
    std::string body;
};

struct IniLayout {
    std::string header; // the default file's first lines, without "; "
    std::vector<IniSection> sections;
};

// [touch]: a finger held on the front touchscreen at a point while its keys or gamepad button are
// held (input.cpp)
struct TouchBinding {
    std::string name;
    float x = 0, y = 0; // 960x544 screen pixels
    std::string keys; // up to two, as in [keyboard]
    std::string gamepad; // one button as in [gamepad], or lefttrigger / righttrigger
    std::string title; // the settings menu's Controls page's name for it (a game's own); empty: from `name`
};

// Every declared setting with its checked value (the default where the file has none or an
// invalid one), and the free sections as written. Keys are "section.key".
class Values {
public:
    const std::string &get(const std::string &key) const;
    bool get_bool(const std::string &key) const;
    int get_int(const std::string &key) const;
    double get_float(const std::string &key) const;
    fs::path get_path(const std::string &key) const; // empty when unset
    const std::map<std::string, std::string> &section(const std::string &name) const;
    bool has_section(const std::string &name) const; // the file has [name]

    std::map<std::string, std::string> values;
    std::map<std::string, std::map<std::string, std::string>> free_sections;
    std::set<std::string> sections_in_file;
};

// the settings every game has, read from the shared declarations (layout.cpp)
struct Settings {
    // [paths]
    fs::path vita_fs; // the Vita file system holding the installed game
    fs::path recompiled; // where ssd_aot_<module>.dll are
    // [display]
    float scale = 1.0f; // internal resolution multiplier
    int window_scale = 1; // window size as a multiple of 960x544
    bool fullscreen = false;
    bool vsync = true;
    std::string filter = "Bilinear";
    std::string fps_counter = "off"; // off, fps, frametime, stats, graph
    bool custom_textures = false; // Vita3K's texture import (TextureCache), off: hashing stays per texture
    // [audio]
    int volume = 100;
    // [recompiled]
    bool use_recompiled = true;
    bool jit = false; // keep the JIT for guest code without native code (NativeCPU otherwise)
    // [debug]
    int log_level = 2; // spdlog: 0 trace ... 6 off
    bool validation_layer = false;
    int surface_sync = 0; // 0 exact, 1 targeted, 2 latent (renderer::State::surface_sync_wait), 3 off
    bool high_accuracy = true;
    std::string framebuffer_reads = "auto"; // auto, subpass, subpass_storage, copy (renderer: VITA3K_FB_READS)
    std::string shader_blend = "auto"; // auto, on, off (renderer: VITA3K_SHADER_BLEND unset, 1, 0)
    bool net_trace = false; // every Ad Hoc packet in the log (net: VITA3K_NET_TRACE=1)
    int gpu = 0; // 0: Vita3K's choice (a discrete GPU first); N: the Nth Vulkan device the log lists
    int hitch_log_ms = 100; // a slower game frame is logged (SceDisplay: VITA3K_HITCH_MS); 0 off
    std::string export_textures = "off"; // off, png, dds: Vita3K's texture export (player 1 only)
    std::string gpu_driver = "auto"; // Android: auto, system, or a libadrenotools package in the app's files/driver/
    // [keyboard] and [gamepad]: Vita button -> keys / gamepad button, applied by input.cpp
    std::map<std::string, std::string> keyboard, gamepad;
    // [touch]
    std::vector<TouchBinding> touches;

    Values values; // everything, the game's own settings included
    std::vector<std::string> warnings; // lines that could not be used
};

// the default file's text
std::string default_ini(const IniLayout &layout);

// the file the settings menu writes: <dir>/<id>.ini, or <dir>/<id>-player<N>.ini for player N >= 2
// (whose lines win over the first)
fs::path settings_file(const fs::path &dir, const std::string &id, int player);

// Sets `key` in [section] of the settings file, keeping everything else as it is: a "key = ..."
// line in that section is replaced (a commented ";key = ..." one when there is no other), or the
// line is added under the section's header; a missing section is added at the end. Returns false
// with the reason in `error`.
bool save_setting(const fs::path &file, const std::string &section, const std::string &key, const std::string &value, std::string &error);

// reads <dir>/<id>.ini (written from the layout when missing), and for player N >= 2 also
// <dir>/<id>-player<N>.ini, whose lines win; then `overrides` ("section.key=value", the command
// line's --set), which win over both, checked as the file's values are, and never saved
Settings load_settings(const fs::path &dir, const std::string &id, const IniLayout &layout,
    const std::vector<TouchBinding> &default_touches, int player = 1,
    const std::vector<std::string> &overrides = {});

// the command line's --help: every setting the layout declares, "section.key" with its default
// and its choices or range
std::string settings_help(const IniLayout &layout);

// The shared declarations, for a game's layout. `exe` is the executable's name (for help texts).
namespace layout {
IniSection paths(const std::string &title_id, const std::string &id, const std::string &exe);
SettingSpec scale();
SettingSpec window_scale();
SettingSpec fullscreen(const std::string &id, const std::string &exe);
SettingSpec vsync();
SettingSpec filter();
// [display] fps_counter: Vita3K's performance overlay (Settings::fps_counter, perf_overlay_level())
SettingSpec fps_counter();
// [display] mipmaps: renderer::State::generate_mips, which the front end sets once the renderer exists
SettingSpec mipmaps(bool on_by_default);
// [display] custom_textures: Vita3K's texture import from <id>/textures/import/<title ID>/ (F9 reads
// it again); its export is [debug] export_textures
SettingSpec custom_textures();
// [game] repatch: the frontend serves ux0/rePatch/<title ID>/ in place of the game's files (repatch.h)
SettingSpec repatch();
IniSection audio();
IniSection recompiled(const std::string &id);
IniSection debug(const std::string &id);
IniSection keyboard(const std::string &extra_help = {});
IniSection gamepad();
// [mouse]: the mouse camera (input.h MouseCamera), for a game whose right stick turns the camera; the
// frontend captures the cursor only in a game whose layout has it. Its hide_cursor (the idle cursor
// hides) is on in a game without the section
IniSection mouse();
// the [touch] section, with a game's default bindings as lines of `body`
IniSection touch(const std::string &body);
// [controls]: the on-screen controls on a phone's touchscreen (Android; the frontend adds the section
// to every game's layout there, after [audio])
IniSection on_screen_controls();
} // namespace layout

} // namespace game_frontend
