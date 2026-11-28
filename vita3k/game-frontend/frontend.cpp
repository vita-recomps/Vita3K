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

// The shared single-game frontend (profile.h): one installed game on the SDL frontend, without the
// Qt interface. Settings that every game needs are fixed here; the player's choices come from
// <id>.ini next to the executable. The recompiled modules (ssd_aot_<module>.dll, generated locally
// from the player's own dump) are loaded from the same folder.

#include "importer.h"
#include "profile.h"

#include "../archive.h"
#include "input.h"
#include "packaged_mods.h"
#include "repatch.h"
#include "settings.h"
#include "settings_menu.h"

#include <app/functions.h>
#include <app/session_controller.h>
#include <audio/state.h>
#include <overlay/display_manager.h>
#include <config/state.h>
#include <config/version.h>
#include <cpu/aot.h>
#include <ctrl/state.h>
#include <display/state.h>
#include <emuenv/state.h>
#include <gxm/state.h>
#include <ime/functions.h>
#include <io/functions.h>
#include <io/state.h>
#include <kernel/state.h>
#include <mem/functions.h>
#include <modules/module_parent.h>
#include <packages/functions.h>
#include <packages/license.h>
#include <packages/pkg.h>
#include <renderer/state.h>
#include <sdl-frontend/session.h>
#include <touch/state.h>
#include <util/log.h>
#include <util/net_utils.h>
#include <util/string_utils.h>
#include <util/sysinfo.h>

#include <rif2zrif.h>
#include <zrif2rif.h>

#ifdef _WIN32
#include <combaseapi.h>
#endif
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

#include <SDL3/SDL.h>
#include <stb_image.h>
#include <stb_image_write.h>

#include <array>
#include <string_view>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace game_frontend {

#ifdef __ANDROID__
void android_window_created(SDL_Window *window); // android.cpp
void android_install_driver_packages(const fs::path &game_dir); // android.cpp
std::string android_gpu_driver(const std::string &setting); // android.cpp
void android_set_emuenv(EmuEnvState *emuenv); // android.cpp
void android_set_session(app::AppSessionController *session); // android.cpp
void android_configure_screen_controls(bool enabled, int size, int opacity, const std::vector<std::pair<float, float>> &spots,
    uint32_t latch_button, const std::string &latch_label);
void android_settings_menu_open(bool open);
#endif

namespace {

// The test driver's gamepad (the input pipe's "gamepad", v3kdrive gpad:): a virtual gamepad of SDL's,
// attached on first use, holding exactly the inputs named. It reaches the game and the settings menu
// as a real one does; from then on gamepads count without the window's focus too.
void hold_test_gamepad(const std::vector<std::string> &names) {
    static SDL_Joystick *joystick = nullptr;
    // the triggers rest at the axis minimum (0 on the gamepad), the sticks at 0
    const auto set_triggers = [](const std::set<int> &held) {
        for (const int axis : { SDL_GAMEPAD_AXIS_LEFT_TRIGGER, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER })
            SDL_SetJoystickVirtualAxis(joystick, axis, held.contains(axis) ? SDL_JOYSTICK_AXIS_MAX : SDL_JOYSTICK_AXIS_MIN);
    };
    if (!joystick) {
        // the test driver never gives the window focus, and SDL ignores gamepads without it
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
        SDL_VirtualJoystickDesc desc;
        SDL_INIT_INTERFACE(&desc);
        desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
        desc.naxes = SDL_GAMEPAD_AXIS_COUNT;
        desc.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
        desc.name = "Test driver gamepad";
        const SDL_JoystickID id = SDL_AttachVirtualJoystick(&desc);
        joystick = id ? SDL_OpenJoystick(id) : nullptr;
        if (!joystick) {
            LOG_WARN("Input pipe: no virtual gamepad ({})", SDL_GetError());
            return;
        }
        set_triggers({});
        LOG_INFO("Input pipe: virtual gamepad attached");
    }
    std::set<int> buttons, axes;
    for (const auto &name : names) {
        const std::string lower = string_utils::tolower(name);
        if (lower == "lefttrigger")
            axes.insert(SDL_GAMEPAD_AXIS_LEFT_TRIGGER);
        else if (lower == "righttrigger")
            axes.insert(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER);
        else if (const int button = gamepad_button_from_name(lower); button >= 0)
            buttons.insert(button);
        else
            LOG_WARN("Input pipe: no gamepad input named '{}'", name);
    }
    for (int button = 0; button < SDL_GAMEPAD_BUTTON_COUNT; button++)
        SDL_SetJoystickVirtualButton(joystick, button, buttons.contains(button));
    set_triggers(axes);
}

void set_env(const char *name, const std::string &value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

#ifdef __ANDROID__
// The debug switches that are environment variables on PCs (SSD_AOT_DIFF, SSD_AOT_COUNT,
// VITA3K_AUDIO_LEVELS, ...): an app gets no environment, so on Android they come from the system
// property debug.vitarecomps.env, NAME=value pairs separated by spaces, set before the game starts
// (adb shell setprop debug.vitarecomps.env 'SSD_AOT_NATIVE=0 SSD_AOT_DIFF=2000'), and from
// debug-env.txt in the game's folder (the same pairs, for values past the property's 91
// characters, such as SSD_AOT_STATS's path). Neither exists unless a tester puts it there.
void apply_android_env(const fs::path &game_dir) {
    const auto apply = [](const std::string &text, const char *from) {
        std::istringstream pairs(text);
        std::string pair;
        while (pairs >> pair) {
            const auto eq = pair.find('=');
            if (eq == std::string::npos || eq == 0)
                continue;
            set_env(pair.substr(0, eq).c_str(), pair.substr(eq + 1));
            LOG_INFO("{}: {}", from, pair);
        }
    };
    char value[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.vitarecomps.env", value) > 0)
        apply(value, "debug.vitarecomps.env");
    std::ifstream file((game_dir / "debug-env.txt").string());
    if (file) {
        std::stringstream text;
        text << file.rdbuf();
        apply(text.str(), "debug-env.txt");
    }
}
#endif

// The last runs' logs: vita3k.log becomes vita3k.1.log (the run before), .1 becomes .2, and so on up
// to vita3k.<kept>.log; the oldest is dropped. A report sent after the game was started again then
// still carries the run that crashed (a launcher sends the whole log folder). A log another copy of
// the game still has open stays where it is.
void keep_previous_logs(const fs::path &dir, int kept) {
    const auto name = [&](int i) { return dir / (i == 0 ? std::string("vita3k.log") : fmt::format("vita3k.{}.log", i)); };
    boost::system::error_code ec;
    if (!fs::exists(name(0), ec))
        return;
    fs::remove(name(kept), ec);
    for (int i = kept - 1; i >= 0; i--) {
        if (fs::exists(name(i), ec))
            fs::rename(name(i), name(i + 1), ec);
    }
}

// SSD_AOT (the fork's list of recompiled modules) from the libraries in `dir` (ssd_aot_*.dll on
// Windows, .dylib on macOS, .so elsewhere), unless it is set already
std::string select_recompiled_modules(const fs::path &dir) {
#ifdef _WIN32
    constexpr const char *extension = ".dll";
#elif defined(__APPLE__)
    constexpr const char *extension = ".dylib";
#else
    constexpr const char *extension = ".so";
#endif
    if (const char *const env = std::getenv("SSD_AOT"))
        return env;
    std::string list;
#ifdef __ANDROID__
    // shared storage (Android/data/<package>/files, where a PC puts the modules) is mounted noexec:
    // dlopen only takes them from the app's internal storage. Copied there when they changed;
    // copies whose module is gone are removed (ss-delta 09-android)
    const fs::path loaded_dir = fs::path(SDL_GetAndroidInternalStoragePath()) / "recompiled";
    if (fs::is_directory(dir) && dir.lexically_normal() != loaded_dir.lexically_normal()) {
        boost::system::error_code ec;
        fs::create_directories(loaded_dir, ec);
        std::set<std::string> present;
        for (const auto &entry : fs::directory_iterator(dir)) {
            const auto name = entry.path().filename().string();
            if (!name.starts_with("ssd_aot_") || entry.path().extension() != extension)
                continue;
            present.insert(name);
            const fs::path copy = loaded_dir / name;
            if (fs::exists(copy) && fs::file_size(copy) == fs::file_size(entry.path())
                && fs::last_write_time(copy) == fs::last_write_time(entry.path()))
                continue;
            fs::copy_file(entry.path(), copy, fs::copy_options::overwrite_existing, ec);
            if (ec) {
                LOG_ERROR("Could not copy {} to {}: {}", entry.path(), copy, ec.message());
                continue;
            }
            fs::last_write_time(copy, fs::last_write_time(entry.path()), ec);
            LOG_INFO("Recompiled module {} copied to internal storage", name);
        }
        for (const auto &entry : fs::directory_iterator(loaded_dir))
            if (!present.contains(entry.path().filename().string()))
                fs::remove(entry.path(), ec);
    }
    const fs::path scan_dir = fs::is_directory(loaded_dir) ? loaded_dir : dir;
#else
    const fs::path &scan_dir = dir;
#endif
    if (fs::is_directory(scan_dir)) {
        for (const auto &entry : fs::directory_iterator(scan_dir)) {
            const auto name = entry.path().filename().string();
            if (name.starts_with("ssd_aot_") && entry.path().extension() == extension)
                list += (list.empty() ? "" : ";") + entry.path().string();
        }
    }
    if (!list.empty())
        set_env("SSD_AOT", list);
    return list;
}

// the frame on screen at render resolution (the internal resolution times `scale`) as a PNG: read
// back on the render thread after its next frame (it alone submits to the GPU queue), written on
// a thread of its own
void save_frame(EmuEnvState &emuenv, const fs::path &path) {
    if (!emuenv.renderer)
        return;
    renderer::State *const renderer = emuenv.renderer.get();
    renderer->run_on_render_thread([renderer, &display = emuenv.display, path] {
        uint32_t width = 0, height = 0;
        std::vector<uint32_t> frame = renderer->dump_frame(display, width, height);
        if (frame.empty() || frame.size() != static_cast<size_t>(width) * height) {
            LOG_WARN("Screenshot: no frame to save");
            return;
        }
        std::thread([frame = std::move(frame), width, height, path]() mutable {
            for (uint32_t &pixel : frame)
                pixel |= 0xFF000000;
            if (path.has_parent_path())
                fs::create_directories(path.parent_path());
            const bool ok = stbi_write_png(fs_utils::path_to_utf8(path).c_str(), width, height, 4, frame.data(), width * 4) == 1;
            LOG_INFO("Screenshot {}x{} {} {}", width, height, ok ? "saved to" : "could not be saved to", path);
        }).detach();
    });
}

// every rendered frame from the next one on, `count` of them, as <dir>/<n>.png (5 digits): for
// glitches that last a frame or two
void record_frames(EmuEnvState &emuenv, const fs::path &dir, int count) {
    if (!emuenv.renderer || count <= 0)
        return;
    renderer::State *const renderer = emuenv.renderer.get();
    auto step = std::make_shared<std::function<void()>>();
    auto left = std::make_shared<int>(count);
    *step = [renderer, &display = emuenv.display, dir, count, left, step] {
        uint32_t width = 0, height = 0;
        std::vector<uint32_t> frame = renderer->dump_frame(display, width, height);
        const int n = count - *left;
        if (!frame.empty() && frame.size() == static_cast<size_t>(width) * height) {
            std::thread([frame = std::move(frame), width, height, path = dir / fmt::format("{:05}.png", n)]() mutable {
                // VITA3K_RECORD_ALPHA=1: keep the alpha channel (debugging aid, with VITA3K_DISPLAY_SURFACE)
                static const bool keep_alpha = [] {
                    const char *v = std::getenv("VITA3K_RECORD_ALPHA");
                    return v && v[0] == '1';
                }();
                if (!keep_alpha)
                    for (uint32_t &pixel : frame)
                        pixel |= 0xFF000000;
                stbi_write_png(fs_utils::path_to_utf8(path).c_str(), width, height, 4, frame.data(), width * 4);
            }).detach();
        }
        if (--*left > 0)
            renderer->run_on_render_thread(*step);
        else
            LOG_INFO("Recorded {} frames to {}", count, dir);
    };
    fs::create_directories(dir);
    renderer->run_on_render_thread(*step);
}

// the content ID a licence file (work.bin, .rif: SceNpDrmLicense) is for, or empty when it can't be read
std::string license_content_id(const std::string &license) {
    fs::ifstream file(fs_utils::utf8_to_path(license), std::ios::binary);
    SceNpDrmLicense rif{};
    if (!file || !file.read(reinterpret_cast<char *>(&rif), sizeof(rif)))
        return {};
    return std::string(rif.content_id, strnlen(rif.content_id, sizeof(rif.content_id)));
}

// a pkg's licence as a zRIF: from --zrif, from a licence file (work.bin, .rif), or, for a patch or
// a game installed before, from the licence already in the Vita file system. A licence file for
// other content (another game's work.bin picked by mistake, ss-delta #25) would only fail the pkg's
// decryption with no word of why: it is refused by name, and the installed licence used when there
// is one. When none fits, `problem` says why in a line for the player (the setup screens show it).
std::string pkg_zrif(const fs::path &pkg, const std::string &zrif, const std::string &license, const fs::path &vita_fs, std::string &problem) {
    if (!zrif.empty())
        return zrif;
    if (!license.empty()) {
        fs::ifstream file(fs_utils::utf8_to_path(license), std::ios::binary | std::ios::ate);
        if (!file) {
            LOG_ERROR("Licence {} could not be read", license);
            problem = "its licence file could not be read.";
            return {};
        }
        const std::string for_id = license_content_id(license);
        const std::string pkg_id = pkg_content_id(pkg);
        if (for_id.empty() || pkg_id.empty() || for_id == pkg_id)
            return rif2zrif(file);
        LOG_ERROR("Licence {} is for {}, not for {} ({}): it can't decrypt it", license, for_id, pkg, pkg_id);
        if (std::string installed = find_pkg_zrif(pkg, vita_fs); !installed.empty()) {
            LOG_WARN("{}: the licence already installed for {} is used instead", pkg, pkg_id);
            return installed;
        }
        LOG_ERROR("{} needs the licence (work.bin or .rif) for {}", pkg, pkg_id);
        problem = fmt::format("the licence picked is for {}, not for this package. Pick the work.bin or .rif for {}.", for_id, pkg_id);
        return {};
    }
    std::string installed = find_pkg_zrif(pkg, vita_fs);
    if (installed.empty())
        problem = "it needs its licence: pick its work.bin or .rif with it.";
    return installed;
}

struct InstallItem {
    std::string path;
    std::string license; // --license after it: work.bin or .rif (a folder's or zip's: the game's, ss-delta #13)
    std::string zrif; // --zrif after it
};

// a folder's or zip's licence given with it, as a file: --license as it is, --zrif written to temp; empty
// when none was given (the dump's own then)
fs::path dump_license_file(const InstallItem &item, const fs::path &temp) {
    if (!item.license.empty())
        return fs_utils::utf8_to_path(item.license);
    if (item.zrif.empty())
        return {};
    fs::create_directories(temp.parent_path());
    fs::ofstream file(temp, std::ios::binary);
    if (!file) {
        LOG_ERROR("{} could not be written", temp);
        return {};
    }
    zrif2rif(item.zrif, file);
    return temp;
}

// [display] fps_counter onto Vita3K's performance overlay (the renderer draws it): off, or its detail
// levels in order (fps, frametime, stats, graph), in the top left corner
void apply_fps_counter(Config &cfg, const std::string &value) {
    static constexpr std::array<std::string_view, 4> levels{ "fps", "frametime", "stats", "graph" };
    const auto it = std::find(levels.begin(), levels.end(), value);
    cfg.performance_overlay = it != levels.end();
    cfg.performance_overlay_detail = cfg.performance_overlay ? static_cast<int>(MINIMUM) + static_cast<int>(it - levels.begin()) : static_cast<int>(MINIMUM);
    cfg.performance_overlay_position = static_cast<int>(TOP_LEFT);
}

} // namespace

// the command line (--help prints it, with every setting)
constexpr const char *USAGE = R"(The game builds itself the first time it starts: its importer (setup/) asks for
your own dump of the game, then the game starts. Everything stays in the game's folder.

  --game <folder>             the game's folder (default: the executable's; macOS: the one holding
                              the app)
  --install                   open the importer even though the game is built (another dump, an
                              update), then play
  --dump <zip|vpk|folder|pkg> [--license <work.bin|rif> | --zrif <zRIF>]
                              build the game from this dump without the importer's window (repeat
                              for its update, add-ons and rePatch mods; also --dump=<path>), then
                              play. With the game built, add-ons and mods alone are installed,
                              without building again; the same dump it was built from: nothing
                              to do (a launcher's setup step run again)
  --rebuild                   with --dump: build again even from the same dump
  --build-only               build the game if it isn't (or with --install, --dump), then quit
                              instead of playing (exit code 0: built)
  --add-to-steam              Linux and the Steam Deck: add the game to Steam (Steam must be
                              running), then quit
  --android                   put the game, built here, on an Android device (its app, files and
                              code built for Android; the importer's window asks how the device is
                              connected), then quit. With --dump: build it for both, then copy it
  --serial <s>, --pair <host:port> --code <code>, --connect <host:port>, --push-only,
  --accept-android-sdk-license, --apk <file>
                              (with --android) no window: the device by adb serial, or paired over
                              Wi-Fi (the address and code its Wireless debugging screen shows);
                              copy again what was built before; accept Google's licence for adb,
                              which the importer downloads; another APK than the release's
  --shortcut, --steam         (with --dump) a desktop shortcut (Windows), the game in Applications
                              (macOS) or the applications menu (Linux); add it to Steam (Linux)
  --cache <folder>, --jobs <n>
                              (with --dump) keep the importer's downloads in that folder for the
                              next time; compile with that many jobs
  --offline <folder|file>     take the importer's downloads (the firmware, the compiler) from there
                              first, matched by size and SHA-256 whatever their names; repeatable.
                              The game folder's offline/ is searched unasked
  --download-only <folder> [--for windows,linux,macos,android|all]
                              everything the importer downloads, for this platform or those, into
                              that folder (then --offline <folder> needs no network), then quit
  --progress                  (with --dump) also print "progress <0-1> <text>" lines, for a
                              launcher's progress bar (RecompLauncher)
  --set <section.key=value>   a setting for this run only, over the settings file (the list below)
  --fullscreen, --windowed    the same as --set display.fullscreen=true / false
  --player <1-4>              (games with Ad Hoc Play) the copy of another player, with a Vita user
                              (saves), config and log of its own
  --title-id <id>             (games released in several regions) that region's game, when more
                              than one is installed
  --vita-fs <folder>          the Vita file system instead of the settings file's
  --write-ini <file>          write the default settings file there, then quit
  --help                      this list
For the importer and scripts: --install-firmware <PUP>, --install-content <dump> [--license ...|
--zrif ...] (install into the Vita file system, then quit; a folder or zip with rePatch/<title ID>/
goes to ux0/rePatch/, read in place of the game's files while [game] repatch is on),
--remove-mod <id> (take a packaged mod out again; the log names those installed),
--dump-elfs <seconds> (run unseen, writing every module as loaded to <id>/elfdumps/, then quit).
)";

int run(Profile &profile, int argc, char *argv[]) {
    // the command line: USAGE above, and the game's own options (its Hooks::parse_arg)
#ifdef __APPLE__
    // a downloaded app Gatekeeper runs from a translocated copy starts again from its own folder
    if (relaunch_from_original_location(argc, argv))
        return 0;
#endif
    Context ctx;
    for (int i = 1; i < argc; i++) {
        // --dump=<path> (and --license=, --zrif=): one argument, as launchers pass an optional file
        // (RecompLauncher leaves out a whole argument whose file wasn't chosen)
        const std::string arg = argv[i];
        const auto eq = arg.find('=');
        const std::string key = eq == std::string::npos ? std::string() : arg.substr(0, eq);
        if (key == "--dump" || key == "--license" || key == "--zrif") {
            if (eq + 1 == arg.size())
                continue; // an empty value: nothing chosen (else it would name the current folder)
            ctx.args.push_back(key);
            ctx.args.push_back(arg.substr(eq + 1));
        } else
            ctx.args.push_back(arg);
    }
    const std::string exe = profile.id + ".exe";

    std::vector<std::string> install_firmware;
    std::vector<InstallItem> install_content;
    std::vector<std::string> remove_mods; // --remove-mod
    double dump_elfs_seconds = 0;
    std::string vita_fs_override;
    std::string title_id_override;
    std::string game_dir_override;
    std::vector<std::string> importer_args; // --dump parts, as the importer's --install parts
    std::vector<std::string> importer_options; // the importer's own options, passed on to it
    std::vector<std::string> overrides; // --set
    std::vector<std::string> android_options; // --android's, passed on to the importer
    bool open_importer = false, add_to_steam = false, help = false, build_only = false, android = false;
    std::string download_only; // --download-only <folder>: the importer's downloads into it, then quit
    bool last_part_is_dump = false; // a --license or --zrif belongs to the part before it
    std::vector<std::string> unknown_args;
    for (size_t i = 0; i < ctx.args.size(); i++) {
        const std::string &arg = ctx.args[i];
        const bool has_value = i + 1 < ctx.args.size();
        if (arg == "--install-firmware" && has_value)
            install_firmware.push_back(ctx.args[++i]);
        else if (arg == "--install-content" && has_value) {
            install_content.push_back({ ctx.args[++i] });
            last_part_is_dump = false;
        } else if (arg == "--remove-mod" && has_value) {
            remove_mods.push_back(ctx.args[++i]);
        } else if (arg == "--dump" && has_value) {
            importer_args.insert(importer_args.end(), { "--install", fs_utils::path_to_utf8(fs::absolute(fs_utils::utf8_to_path(ctx.args[++i]))) });
            last_part_is_dump = true;
        } else if ((arg == "--license" || arg == "--zrif") && has_value && last_part_is_dump) {
            const std::string &value = ctx.args[++i];
            importer_args.insert(importer_args.end(), { arg, arg == "--license" ? fs_utils::path_to_utf8(fs::absolute(fs_utils::utf8_to_path(value))) : value });
        } else if ((arg == "--license" || arg == "--zrif") && has_value && !install_content.empty())
            (arg == "--license" ? install_content.back().license : install_content.back().zrif) = ctx.args[++i];
        else if (arg == "--install")
            open_importer = true;
        else if (arg == "--accept-microsoft-license" || arg == "--shortcut" || arg == "--steam" || arg == "--progress" || arg == "--rebuild")
            importer_options.push_back(arg);
        else if ((arg == "--cache" || arg == "--jobs" || arg == "--offline" || arg == "--for") && has_value)
            importer_options.insert(importer_options.end(), { arg, arg == "--cache" || arg == "--offline" ? fs_utils::path_to_utf8(fs::absolute(fs_utils::utf8_to_path(ctx.args[i + 1]))) : ctx.args[i + 1] }), i++;
        else if (arg == "--download-only" && has_value)
            download_only = fs_utils::path_to_utf8(fs::absolute(fs_utils::utf8_to_path(ctx.args[++i])));
        else if (arg == "--add-to-steam")
            add_to_steam = true;
        else if (arg == "--android")
            android = true;
        else if (arg == "--accept-android-sdk-license" || arg == "--push-only")
            android_options.push_back(arg);
        else if ((arg == "--serial" || arg == "--pair" || arg == "--code" || arg == "--connect" || arg == "--apk") && has_value)
            android_options.insert(android_options.end(), { arg, arg == "--apk" ? fs_utils::path_to_utf8(fs::absolute(fs_utils::utf8_to_path(ctx.args[i + 1]))) : ctx.args[i + 1] }), i++;
        else if (arg == "--build-only")
            build_only = true;
        else if (arg == "--game" && has_value)
            game_dir_override = ctx.args[++i];
        else if (arg == "--set" && has_value)
            overrides.push_back(ctx.args[++i]);
        else if (arg == "--fullscreen" || arg == "--windowed")
            overrides.push_back(arg == "--fullscreen" ? "display.fullscreen=true" : "display.fullscreen=false");
        else if (arg == "--help" || arg == "-h" || arg == "/?")
            help = true;
        else if (arg == "--dump-elfs" && has_value)
            dump_elfs_seconds = std::atof(ctx.args[++i].c_str());
        else if (arg == "--vita-fs" && has_value)
            vita_fs_override = ctx.args[++i];
        else if (arg == "--title-id" && has_value && !profile.other_title_ids.empty())
            title_id_override = ctx.args[++i];
        else if (arg == "--write-ini" && has_value) {
            // the default settings file, for packaging scripts; nothing else runs
            const fs::path path = fs_utils::utf8_to_path(ctx.args[++i]);
            fs::ofstream(path) << default_ini(profile.ini);
            return fs::exists(path) ? 0 : 1;
        } else if (arg == "--player" && has_value)
            ctx.player = std::clamp(std::atoi(ctx.args[++i].c_str()), 1, net_utils::LOCAL_ADHOC_MAX);
        else if (!profile.hooks.parse_arg || !profile.hooks.parse_arg(ctx, ctx.args, i))
            unknown_args.push_back(arg);
    }
    // player N is the Vita user N-1 (00 for player 1, the user every install has). A game without Ad Hoc
    // Play takes --player N as another copy for test drivers (runs in parallel, each started from a folder
    // of its own): its own window, input pipe, config and log, the same Vita user
    ctx.user_id = profile.multiplayer ? fmt::format("{:02}", ctx.player - 1) : "00";
    ctx.window_name = ctx.player == 1 ? profile.name : fmt::format("{} (player {})", profile.name, ctx.player);
    const auto fail = [&](const std::string &message) {
        LOG_ERROR("{}", message);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, ctx.window_name.c_str(), message.c_str(), nullptr);
    };

    if (help) {
        attach_parent_console();
        fmt::print("{} ({})\n\n{}{}\nSettings (--set section.key=value; keys and sections also in {}.ini):\n{}",
            profile.name, profile.id, USAGE, profile.usage.empty() ? "" : "\n" + profile.usage, profile.id,
            settings_help(profile.ini));
        std::fflush(stdout);
        return 0;
    }

    // Everything in the game's folder (ss-delta D22): --game, else the folder an app copy elsewhere
    // names (macOS ~/Applications: LSEnvironment), else the executable's (macOS: the one holding the
    // app, SDL_FILESYSTEM_BASE_DIR_TYPE). Nothing in the user's profile (no SDL_GetPrefPath).
    fs::path game_dir;
    if (!game_dir_override.empty())
        game_dir = fs::absolute(fs_utils::utf8_to_path(game_dir_override));
    else if (const char *env = std::getenv("VITA_RECOMPS_GAME_DIR"); env && *env)
        game_dir = fs_utils::utf8_to_path(env);
#ifdef __ANDROID__
    // Android: the app's folder on shared storage (Android/data/<package>/files), which a PC reaches
    // over USB or adb (ss-delta 09-android)
    else if (const char *external = SDL_GetAndroidExternalStoragePath())
        game_dir = fs_utils::utf8_to_path(external);
#else
    else if (const char *base = SDL_GetBasePath())
        game_dir = fs_utils::utf8_to_path(base);
#endif
    // (Boost keeps a trailing separator as a last element ".")
    game_dir = game_dir.lexically_normal();
    while ((game_dir.filename() == "." || game_dir.filename().empty()) && game_dir.has_parent_path() && game_dir.parent_path() != game_dir)
        game_dir = game_dir.parent_path();
    game_dir /= "";
    Root root_paths;
#ifdef __ANDROID__
    // data/ and shaders-builtin/ are the APK's assets, which SDL opens by their relative paths
    root_paths.set_static_assets_path({});
#else
    root_paths.set_static_assets_path(game_dir);
#endif
    root_paths.set_cache_path(game_dir / "cache" / "");
    root_paths.set_patch_path(game_dir / "patch" / "");
    ctx.exe_dir = game_dir;
    // our own config and log, apart from a Vita3K.exe in the same folder. Players 2 and up have theirs
    // in a folder of their own.
    ctx.data_dir = ctx.player == 1 ? ctx.exe_dir / profile.id / "" : ctx.exe_dir / profile.id / fmt::format("player{}", ctx.player) / "";
    fs::create_directories(ctx.data_dir);
    const fs::path step_error = ctx.data_dir / "step-error.txt";
    if (!install_firmware.empty() || !install_content.empty() || !remove_mods.empty()) {
        boost::system::error_code ec;
        fs::remove(step_error, ec);
    }
    root_paths.set_config_path(ctx.data_dir);
    root_paths.set_log_path(ctx.data_dir);
    root_paths.set_shared_path(ctx.data_dir);

#ifdef __ANDROID__
    // the on-screen controls' settings, for every game: after [audio] (its settings menu page follows Audio)
    {
        auto &sections = profile.ini.sections;
        const auto audio = std::find_if(sections.begin(), sections.end(), [](const IniSection &s) { return s.name == "audio"; });
        sections.insert(audio == sections.end() ? sections.end() : audio + 1, layout::on_screen_controls());
    }
#endif
    const auto read_settings = [&] {
        Settings s = load_settings(ctx.exe_dir, profile.id, profile.ini, profile.default_touches, ctx.player, overrides);
        if (!vita_fs_override.empty())
            s.vita_fs = fs_utils::utf8_to_path(vita_fs_override) / "";
        return s;
    };
    Settings settings = read_settings();

    // Not built yet (or --install, --dump, --add-to-steam): the importer in setup/, waited for. The
    // importer's own runs of this executable (--install-content, --dump-elfs) never get here.
    if (install_firmware.empty() && install_content.empty() && remove_mods.empty() && dump_elfs_seconds <= 0) {
        const auto built = [&] {
            if (fs::exists(settings.vita_fs / "ux0" / "app" / profile.title_id / "eboot.bin"))
                return true;
            return std::any_of(profile.other_title_ids.begin(), profile.other_title_ids.end(),
                [&](const std::string &id) { return fs::exists(settings.vita_fs / "ux0" / "app" / id / "eboot.bin"); });
        };
        if (!download_only.empty()) {
            // everything the importer downloads, into a folder it uses later without the network
            if (!has_importer(game_dir)) {
                fail(fmt::format("{} has no importer (setup/).", fs_utils::path_to_utf8(game_dir)));
                return 1;
            }
            attach_parent_console();
            std::vector<std::string> args{ "--download-only", download_only };
            args.insert(args.end(), importer_options.begin(), importer_options.end());
            args.insert(args.end(), android_options.begin(), android_options.end());
            const int code = run_importer(game_dir, own_executable(), args, true);
            return code < 0 ? 1 : code;
        }
        if (add_to_steam || android || open_importer || !importer_args.empty() || !built()) {
            if (has_importer(game_dir)) {
                // --android's device options (anything but --apk and --push-only) mean no window
                const bool device_given = std::any_of(android_options.begin(), android_options.end(), [](const std::string &o) {
                    return o == "--serial" || o == "--pair" || o == "--connect";
                });
                const bool console = add_to_steam || !importer_args.empty() || (android && device_given);
                if (console)
                    attach_parent_console();
                std::vector<std::string> args = add_to_steam ? std::vector<std::string>{ "--add-to-steam" } : importer_args;
                if (!add_to_steam)
                    args.insert(args.end(), importer_options.begin(), importer_options.end());
                if (android) {
                    args.push_back("--android");
                    args.insert(args.end(), android_options.begin(), android_options.end());
                    if (console && importer_args.empty())
                        args.push_back("--console");
                }
                const int code = run_importer(game_dir, own_executable(), args, console);
                if (android)
                    return code == 2 ? 0 : code < 0 ? 1 : code;
                if (add_to_steam || code != 0) {
                    if (code < 0)
                        fail(fmt::format("The importer in {} could not be started.", fs_utils::path_to_utf8(game_dir / "setup")));
                    // 2: built, but the importer's window was closed without Play
                    return code == 2 ? 0 : code < 0 ? 1 : code;
                }
                if (build_only)
                    return 0;
                settings = read_settings();
            } else if (add_to_steam || android || open_importer || !importer_args.empty()) {
                fail(fmt::format("{} has no importer (setup/): download the release again to build the game.",
                    fs_utils::path_to_utf8(game_dir)));
                return 1;
            }
        }
        if (build_only)
            return built() ? 0 : 1;
    }
    root_paths.set_vita_fs_path(settings.vita_fs);
    // (Vita3K reads both without checking they exist)
    fs::create_directories(root_paths.get_cache_path());
    fs::create_directories(root_paths.get_patch_path());
    ctx.settings = &settings;

    keep_previous_logs(ctx.data_dir, 4); // five in all, with this run's
    if (logging::init(root_paths, false) != Success)
        return 1;
    logging::set_level(static_cast<spdlog::level::level_enum>(settings.log_level));
    LOG_INFO("{} on {}", profile.id, window_title);
    LOG_INFO_IF(ctx.player > 1, "Player {} (Vita user {})", ctx.player, ctx.user_id);
    LOG_INFO("CPU: {}", util::get_system_info());
    for (const auto &line : settings.warnings)
        LOG_WARN("{}.ini: ignored \"{}\"", profile.id, line);
    for (const auto &arg : unknown_args)
        LOG_WARN("Command line: ignored \"{}\"", arg);

    // a game released in several regions: the installed one runs (title_id first), or --title-id's
    std::vector<std::string> title_ids{ profile.title_id };
    title_ids.insert(title_ids.end(), profile.other_title_ids.begin(), profile.other_title_ids.end());
    if (!profile.other_title_ids.empty()) {
        const auto installed = [&](const std::string &id) { return fs::exists(settings.vita_fs / "ux0" / "app" / id / "eboot.bin"); };
        const auto known = [&](const std::string &id) {
            return id == profile.title_id || std::find(profile.other_title_ids.begin(), profile.other_title_ids.end(), id) != profile.other_title_ids.end();
        };
        if (!title_id_override.empty()) {
            if (known(title_id_override))
                profile.title_id = title_id_override;
            else
                LOG_WARN("--title-id {}: not one of this game's title IDs; {} stays", title_id_override, profile.title_id);
        } else if (!installed(profile.title_id)) {
            for (const auto &id : profile.other_title_ids) {
                if (installed(id)) {
                    profile.title_id = id;
                    break;
                }
            }
        }
        LOG_INFO("Title: {}", profile.title_id);
    }

    if (profile.hooks.before_init) {
        if (const auto exit_code = profile.hooks.before_init(ctx))
            return *exit_code;
    }

#ifdef __ANDROID__
    // Vulkan driver packages a PC put in drivers/, into the app's internal storage ([debug] gpu_driver)
    android_install_driver_packages(ctx.exe_dir);
    apply_android_env(ctx.exe_dir);
#endif
    // --dump-elfs: no recompiled modules, everything on the JIT, as the desktop setup has always run it
    // (install-game.py's environment). SSD_AOT also makes fiber switches go through SceFiber's HLE
    // (load_module.cpp), as in play with native code: libfiber isn't loaded, and the modules after it
    // are dumped at the addresses they take in play (on Android, which has no such environment,
    // libfiber loaded and moved libult and the later modules)
    if (dump_elfs_seconds > 0) {
        if (!std::getenv("SSD_AOT"))
            set_env("SSD_AOT", "none");
        if (!std::getenv("SSD_AOT_NATIVE"))
            set_env("SSD_AOT_NATIVE", "0");
    }
    if (settings.use_recompiled) {
        const std::string modules = select_recompiled_modules(settings.recompiled);
        LOG_INFO_IF(!modules.empty(), "Recompiled modules: {}", modules);
        LOG_WARN_IF(modules.empty(), "No recompiled modules in {}: everything runs on the JIT", settings.recompiled);
        // the native CPU (no JIT) unless [recompiled] jit = true; SSD_AOT_NATIVE set beforehand wins
        if (!modules.empty() && !settings.jit && !std::getenv("SSD_AOT_NATIVE"))
            set_env("SSD_AOT_NATIVE", "1");
        LOG_INFO("CPU: {}", !modules.empty() && std::getenv("SSD_AOT_NATIVE") && std::string(std::getenv("SSD_AOT_NATIVE")) == "1"
                ? "native code only (NativeCPU)" : "JIT with native code");
    }

    Config cfg{};
    cfg.frontend = Frontend::sdl;
    cfg.run_app_path = profile.title_id;
    cfg.config_path = ctx.data_dir / "config.yml";
    cfg.set_vita_fs_path(settings.vita_fs);
    cfg.log_level = settings.log_level;
    cfg.discord_rich_presence = false;
    // the games' online services are gone: no network at all, as on a Vita in flight mode
    cfg.offline = true;
    cfg.check_for_updates = false;
    // Vulkan, surface sync and high accuracy, double-buffered memory mapping (ss-delta D4 update 4);
    // [debug] surface_sync and high_accuracy, or a game's configure hook, can change them
    cfg.backend_renderer = "Vulkan";
    cfg.high_accuracy = settings.high_accuracy;
    cfg.gpu_idx = settings.gpu;
    // [debug] framebuffer_reads, read by the Vulkan renderer (ss-delta #17); an environment value set by hand wins
    if (!std::getenv("VITA3K_FB_READS"))
        set_env("VITA3K_FB_READS", settings.framebuffer_reads);
    // [debug] shader_blend: auto leaves the renderer's choice
    if (!std::getenv("VITA3K_SHADER_BLEND") && settings.shader_blend != "auto")
        set_env("VITA3K_SHADER_BLEND", settings.shader_blend == "on" ? "1" : "0");
    // [debug] net_trace: P2PSocket logs each Ad Hoc packet (ss-delta #21)
    if (!std::getenv("VITA3K_NET_TRACE") && settings.net_trace)
        set_env("VITA3K_NET_TRACE", "1");
    if (const char *trace = std::getenv("VITA3K_NET_TRACE"); trace && trace[0] == '1')
        LOG_INFO("Ad Hoc Play: every packet sent and received is logged ([debug] net_trace)");
    // [debug] hitch_log_ms: SceDisplay logs a slower game frame; read once, before the game starts
    if (!std::getenv("VITA3K_HITCH_MS"))
        set_env("VITA3K_HITCH_MS", std::to_string(settings.hitch_log_ms));
    cfg.disable_surface_sync = settings.surface_sync == 3;
    cfg.memory_mapping = "double-buffer";
#ifdef __ANDROID__
    // a Turnip build or another libadrenotools package in the app's files/driver/ (ss-delta 09-android)
    cfg.custom_driver_name = android_gpu_driver(settings.gpu_driver);
    LOG_INFO("Vulkan driver: {} ([debug] gpu_driver = {})", cfg.custom_driver_name.empty() ? "the system's" : cfg.custom_driver_name, settings.gpu_driver);
#endif
    // a pipeline seen for the first time is compiled in the background (its draws are skipped
    // meanwhile) and kept in the pipeline cache next to the executable; no on-screen notice
    cfg.async_pipeline_compilation = true;
    cfg.show_compile_shaders = false;
    cfg.validation_layer = settings.validation_layer;
    // the player's choices
    cfg.resolution_multiplier = settings.scale;
    cfg.v_sync = settings.vsync;
    cfg.screen_filter = settings.filter;
    apply_fps_counter(cfg, settings.fps_counter);
    // --dump-elfs runs the game unseen (a setup's step): no sound either
    cfg.audio_volume = dump_elfs_seconds > 0 ? 0 : settings.volume;
    if (profile.hooks.configure)
        profile.hooks.configure(ctx, cfg);

    // windows without an icon of their own get the executable's (its .rc)
    if (profile.icon_resource) {
        SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "101");
        SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "101");
    }
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS, "1");

#ifdef _WIN32
    LOG_ERROR_IF(CoInitializeEx(nullptr, COINIT_MULTITHREADED) == S_FALSE, "Failed to initialize COM Library");
#endif

    EmuEnvState emuenv;
    ctx.emuenv = &emuenv;
#ifdef __ANDROID__
    // the on-screen controls reach the game's state from the time its window exists (on_window) until
    // run() leaves, before emuenv goes
    struct ScreenControlsLink {
        ~ScreenControlsLink() { android_set_emuenv(nullptr); }
    } screen_controls_link;
#endif
    if (!app::init(emuenv, cfg, root_paths)) {
        fail(fmt::format("The emulated environment could not be initialised. See {}/vita3k.log.", profile.id));
        return 1;
    }
    if (emuenv.cfg.controller_binds.size() != 15 || emuenv.cfg.controller_axis_binds.size() != 6)
        app::reset_controller_binding(emuenv);
    {
        std::vector<std::string> binding_warnings;
        apply_bindings(emuenv, settings.keyboard, settings.gamepad, binding_warnings);
        for (const auto &line : binding_warnings)
            LOG_WARN("{}.ini: {}", profile.id, line);
    }

    // SSD_DUMP_ELFS: write every module as loaded (decrypted, relocated) to <id>/elfdumps/, the
    // input for recompiling the firmware modules the game loads
    if (std::getenv("SSD_DUMP_ELFS") || dump_elfs_seconds > 0)
        emuenv.kernel.debugger.dump_elfs = true;

    init_libraries(emuenv);

    if (!install_firmware.empty() || !install_content.empty() || !remove_mods.empty()) {
        bool ok = true;
        // a line per part that failed, for the player: written to <id>/step-error.txt beside the log,
        // which the setup screens (setup.py, Android's BuildActivity) show instead of an exit code
        std::vector<std::string> problems;
        const auto failed = [&](const std::string &item, const std::string &why) {
            problems.push_back(fmt::format("{}: {}", fs_utils::path_to_utf8(fs_utils::utf8_to_path(item).filename()), why));
        };
        for (const auto &pup : install_firmware) {
            LOG_INFO("Installing firmware {}", pup);
            fs::create_directories(emuenv.vita_fs_path);
            install_pup(emuenv.vita_fs_path, fs_utils::utf8_to_path(pup), [](uint32_t progress) {
                LOG_INFO("Firmware installation: {}%", progress);
            });
        }
        for (const auto &item : install_content) {
            const fs::path path = fs_utils::utf8_to_path(item.path);
            LOG_INFO("Installing {}", item.path);
            // a part that fails without a reason of its own gets the log's
            const bool ok_before = ok;
            const size_t problems_before = problems.size();
            const auto note_failure = [&] {
                if (ok_before && !ok && problems.size() == problems_before)
                    failed(item.path, "could not be installed; the game's log says why.");
            };
            // a dump that carries a rePatch mod beside the game's own content: the content first (below),
            // then the mod (after it), rather than the mod alone
            const bool content_and_mod = holds_game_content(path);
            // a packaged mod (packaged_mods.h): merged into the archive it changes
            if (const auto mod = install_packaged_mod(emuenv.vita_fs_path, path, title_ids)) {
                LOG_ERROR_IF(!*mod, "{}: the mod could not be installed", item.path);
                ok = *mod && ok;
            }
            // a rePatch mod (repatch.h): beside the game, not into it
            else if (const auto mod = content_and_mod ? std::nullopt : install_repatch(emuenv.vita_fs_path, path, title_ids)) {
                LOG_ERROR_IF(!*mod, "{}: the mod could not be installed", item.path);
                ok = *mod && ok;
            } else if (fs::is_directory(path)) {
                // every content in the folder, not just one: a game that failed beside an add-on that
                // installed is a failed install (ss-delta #13)
                const fs::path temp = emuenv.cache_path / "dump_licence.rif";
                uint32_t found = 0;
                const uint32_t installed = install_contents(emuenv, path, dump_license_file(item, temp), &found);
                fs::remove(temp);
                LOG_ERROR_IF(installed < found, "{}: {} of the {} contents in it could not be installed", item.path,
                    found - installed, found);
                ok = installed > 0 && installed == found && ok;
            } else if (string_utils::tolower(path.extension().string()) == ".pkg") {
                std::string problem;
                std::string zrif = pkg_zrif(path, item.zrif, item.license, emuenv.vita_fs_path, problem);
                if (zrif.empty()) {
                    LOG_ERROR("{}: no licence that fits it (give --license <work.bin> or --zrif after it)", item.path);
                    failed(item.path, problem.empty() ? "no licence that fits it." : problem);
                    ok = false;
                    continue;
                }
                int last = -1;
                const bool installed = install_pkg(path, emuenv, zrif, [&last](float progress) {
                    if (static_cast<int>(progress) / 10 != last) {
                        last = static_cast<int>(progress) / 10;
                        LOG_INFO("Installation: {}%", static_cast<int>(progress));
                    }
                });
                LOG_ERROR_IF(!installed, "{} could not be installed", item.path);
                ok = installed && ok;
            } else {
                const fs::path temp = emuenv.cache_path / "dump_licence.rif";
                const auto installed = install_archive(emuenv, path, nullptr, nullptr, dump_license_file(item, temp));
                fs::remove(temp);
                ok = !installed.empty() && std::all_of(installed.begin(), installed.end(), [](const ContentInfo &c) { return c.state; }) && ok;
            }
            if (content_and_mod) {
                if (const auto mod = install_repatch(emuenv.vita_fs_path, path, title_ids)) {
                    LOG_ERROR_IF(!*mod, "{}: its rePatch mod could not be installed", item.path);
                    ok = *mod && ok;
                }
            }
            note_failure();
        }
        for (const auto &id : remove_mods)
            ok = remove_packaged_mod(emuenv.vita_fs_path, id, title_ids) && ok;
        // packaged mods merged again into an archive a rePatch mod just replaced (an undub's)
        if (!install_content.empty())
            for (const auto &id : title_ids)
                if (fs::is_directory(emuenv.vita_fs_path / "ux0" / "rePatch" / id))
                    refresh_packaged_mods(emuenv.vita_fs_path, id);
        LOG_INFO("{} {}", remove_mods.empty() || !install_content.empty() ? "Installation" : "Removal", ok ? "done" : "failed");
        if (!ok && !problems.empty()) {
            fs::ofstream file(step_error, std::ios::binary);
            for (const auto &line : problems)
                file << line << '\n';
        }
#ifdef _WIN32
        CoUninitialize();
#endif
        return ok ? 0 : 1;
    }
    if (!app::init_apps_list(emuenv)) {
        fail(fmt::format("The list of installed apps could not be read. See {}/vita3k.log.", profile.id));
        return 1;
    }
    app::load_users(emuenv);
    if (ctx.player > 1 && profile.multiplayer) {
        // users 01, 02 ... in order, so that player N's is N-1
        for (int p = 2; p <= ctx.player; p++) {
            const std::string id = fmt::format("{:02}", p - 1);
            if (!emuenv.app.user_list.users.contains(id) && app::create_user(emuenv, fmt::format("Player {}", p)) != id)
                LOG_WARN("Vita user {} could not be created for player {}", id, p);
        }
        if (!emuenv.app.user_list.users.contains(ctx.user_id)) {
            fail(fmt::format("Player {}'s Vita user ({}) could not be created. See {}.", ctx.player, ctx.user_id,
                fs_utils::path_to_utf8(ctx.data_dir / "vita3k.log")));
            return 1;
        }
        emuenv.cfg.user_id = ctx.user_id;
    }

    if (!fs::exists(emuenv.vita_fs_path / "ux0" / "app" / profile.title_id / "eboot.bin")) {
#ifdef __ANDROID__
        // the files come from a PC (the game's setup there: android.py push), not from an ini edit
        fail(fmt::format("{} ({}) is not installed in {}.\nSet the game up on a PC and copy it to this device "
                         "from there (the game's install tools: android.py push).",
            profile.name, profile.title_id, emuenv.vita_fs_path.string()));
#else
        fail(fmt::format("{} ({}) is not installed in {}.\nSet [paths] vita_fs in {}.ini.",
            profile.name, profile.title_id, emuenv.vita_fs_path.string(), profile.id));
#endif
        return 1;
    }

    if (profile.hooks.before_mods)
        profile.hooks.before_mods(ctx, settings.values.get_bool("game.repatch"));

    // [game] repatch (layout::repatch; false in a game without it): mods in ux0/rePatch/<title ID>/,
    // before the game's hook, which may serve copies of the game's files made from them
    if (settings.values.get_bool("game.repatch"))
        apply_repatch(emuenv, profile.title_id);
    else if (fs::is_directory(emuenv.vita_fs_path / "ux0" / "rePatch" / profile.title_id))
        LOG_INFO("rePatch: mods off ([game] repatch = false): the game's own files are read");

    if (profile.hooks.before_run)
        profile.hooks.before_run(ctx);

    // window: size and fullscreen from the settings, F11 or Alt+Enter switch, frame rate in the title
    SDL_Window *window = nullptr;
    Keyboard keyboard;
    Touches touches;
    {
        std::vector<std::string> touch_warnings;
        touches.configure(settings.touches, touch_warnings, profile.touch_targets);
        sync_claimed_gamepad_buttons(emuenv, touches.claimed_gamepad_buttons());
        for (const auto &line : touch_warnings)
            LOG_WARN("{}.ini: {}", profile.id, line);
    }
    // [mouse] (layout::mouse, in a game whose layout has it): middle click or its capture key captures
    // the cursor, and the mouse turns the camera (MouseCamera) until the same again or Esc releases it;
    // the settings menu, a text dialog or another window release it while they last
    const bool has_mouse_camera = settings.values.values.contains("mouse.camera");
    MouseCamera mouse_camera;
    std::vector<input::PhysicalKeyCode> capture_keys;
    bool capture_wanted = false; // the player captured the cursor
    bool captured = false; // SDL's relative mouse mode is on
    int user_marks = 0; // F5's lines in the log
    const auto configure_mouse = [&](bool report) {
        if (!has_mouse_camera)
            return;
        mouse_camera.configure(profile.stick_response, profile.stick_response_y.value_or(profile.stick_response),
            { static_cast<float>(settings.values.get_float("mouse.sensitivity")), settings.values.get_bool("mouse.invert_y") },
            profile.camera_angles, profile.mouse_turn);
        std::vector<std::string> warnings;
        capture_keys = parse_keys(settings.values.get("mouse.capture_key"), "mouse.capture_key", warnings);
        if (settings.values.get_bool("mouse.camera")) {
            const auto uses_key = [&](input::PhysicalKeyCode key, const std::string &list) {
                std::vector<std::string> ignored;
                const auto codes = parse_keys(list, "", ignored);
                return std::find(codes.begin(), codes.end(), key) != codes.end();
            };
            std::erase_if(capture_keys, [&](input::PhysicalKeyCode key) {
                const bool bound = std::any_of(button_bindings().begin(), button_bindings().end(), [&](const auto &button) {
                    const auto it = settings.keyboard.find(button.name);
                    return uses_key(key, it == settings.keyboard.end() ? button.keys : it->second);
                }) || std::any_of(settings.touches.begin(), settings.touches.end(), [&](const auto &touch) {
                    return uses_key(key, touch.keys);
                });
                if (bound)
                    warnings.push_back("mouse.capture_key: " + key_name(key) + " already has a game binding. Middle-click can still capture the cursor.");
                return bound;
            });
        }
        if (report || !warnings.empty())
            for (const auto &line : warnings)
                LOG_WARN("{}.ini: {}", profile.id, line);
        if (!settings.values.get_bool("mouse.camera"))
            capture_wanted = false;
    };
    configure_mouse(true);
    const auto mouse_camera_on = [&] { return has_mouse_camera && settings.values.get_bool("mouse.camera"); };
    // [mouse] hide_cursor (on in a game without [mouse]): the cursor hides once the mouse has been
    // still for a few seconds, or at once on a key or gamepad input, and shows again when the mouse
    // moves or clicks. Apart from the mouse camera's capture, which hides it as well (issue #16)
    bool cursor_hidden = false;
    auto last_mouse_use = std::chrono::steady_clock::now();
    const auto hide_cursor_on = [&] {
#ifdef __ANDROID__
        return false;
#else
        return !settings.values.values.contains("mouse.hide_cursor") || settings.values.get_bool("mouse.hide_cursor");
#endif
    };
    const auto set_cursor_hidden = [&](bool hide) {
        if (hide == cursor_hidden)
            return;
        cursor_hidden = hide;
        if (hide)
            SDL_HideCursor();
        else
            SDL_ShowCursor();
    };
    const auto mouse_used = [&] {
        last_mouse_use = std::chrono::steady_clock::now();
        set_cursor_hidden(false);
    };
    const auto other_input_used = [&] {
        if (hide_cursor_on())
            set_cursor_hidden(true);
    };
    // The settings menu (settings_menu.h): F1, or Select held for a second on a gamepad, opens it
    // over the paused game; F1, Esc or Circle closes it, and its Exit page quits the game. Its changes
    // are written to the settings file here, on the main thread, and the live ones applied.
    app::AppSessionController *session_ptr = nullptr; // set once the session exists, below
    std::shared_ptr<SettingsMenu> menu;
    const auto toggle_capture = [&](input::PhysicalKeyCode key) {
        if (!mouse_camera_on() || menu
            || std::find(capture_keys.begin(), capture_keys.end(), key) == capture_keys.end())
            return false;
        capture_wanted = !capture_wanted;
        return true;
    };
    bool select_held = false, select_used = false;
    std::chrono::steady_clock::time_point select_down{};
#ifdef __ANDROID__
    // the on-screen controls (android.cpp, OnScreenControls.java): [controls], the game's touch spots
    // they leave clear, the [touch] bindings' points (not those a game moves each frame: touch_targets),
    // and the game's latch button
    const auto configure_screen_controls = [&] {
        std::vector<std::pair<float, float>> spots;
        for (const auto &touch : settings.touches)
            if (!profile.touch_targets.contains(touch.name))
                spots.emplace_back(touch.x, touch.y);
        android_configure_screen_controls(settings.values.get_bool("controls.on_screen"),
            settings.values.get_int("controls.on_screen_size"), settings.values.get_int("controls.on_screen_opacity"), spots,
            profile.screen_latch.button, profile.screen_latch.label);
    };
#endif
    const auto apply_change = [&](const SettingsMenu::Change &change) {
        std::string error;
        if (!save_setting(settings_file(ctx.exe_dir, profile.id, ctx.player), change.section, change.key, change.value, error))
            LOG_ERROR("Settings menu: {}", error);
        LOG_INFO("Settings menu: [{}] {} = {}", change.section, change.key, change.value);
        // the Controls page: [keyboard], [gamepad] and [touch], applied at once
        if (change.section == "keyboard" || change.section == "gamepad" || change.section == "touch") {
            settings.values.free_sections[change.section][change.key] = change.value;
            std::vector<std::string> warnings;
            if (change.section == "touch") {
                const bool keys = change.key.ends_with("_key");
                const std::string name = change.key.substr(0, change.key.size() - (keys ? 4 : 8));
                for (auto &touch : settings.touches)
                    if (touch.name == name)
                        (keys ? touch.keys : touch.gamepad) = change.value;
                touches.configure(settings.touches, warnings, profile.touch_targets);
                if (!keys)
                    sync_claimed_gamepad_buttons(emuenv, touches.claimed_gamepad_buttons());
            } else {
                const bool is_keyboard = change.section == "keyboard";
                (is_keyboard ? settings.keyboard : settings.gamepad)[change.key] = change.value;
                const std::map<std::string, std::string> line{ { change.key, change.value } };
                apply_bindings(emuenv, is_keyboard ? line : std::map<std::string, std::string>{},
                    is_keyboard ? std::map<std::string, std::string>{} : line, warnings);
                if (!is_keyboard)
                    sync_claimed_gamepad_buttons(emuenv, touches.claimed_gamepad_buttons());
                keyboard.refresh(emuenv);
            }
            if (change.section != "gamepad")
                configure_mouse(false);
            for (const auto &warning : warnings)
                LOG_WARN("Settings menu: {}", warning);
            return;
        }
        settings.values.values[change.section + "." + change.key] = change.value;
        const std::string key = change.section + "." + change.key;
        if (key == "display.fullscreen" && window)
            SDL_SetWindowFullscreen(window, change.value == "true");
        else if (key == "display.window_scale" && window && !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN)) {
            const int n = std::max(1, std::atoi(change.value.c_str()));
            SDL_SetWindowSize(window, 960 * n, 544 * n);
            SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        } else if (key == "display.vsync" && emuenv.renderer)
            emuenv.renderer->set_vsync_state(change.value == "true");
        else if (key == "display.filter" && emuenv.renderer)
            emuenv.renderer->set_screen_filter(change.value);
        else if (key == "display.fps_counter") {
            apply_fps_counter(emuenv.cfg, change.value);
            app::sync_perf_overlay_config(emuenv);
        }
        else if (key == "audio.volume")
            emuenv.audio.set_global_volume(std::clamp(std::atoi(change.value.c_str()), 0, 100) / 100.f);
        else if (key == "multiplayer.peers" && !net_utils::local_adhoc_player())
            net_utils::set_lan_peers_text(change.value); // the next broadcast goes to them (a game's lan mode)
        else if (change.section == "mouse") {
            configure_mouse(false);
            if (!hide_cursor_on())
                set_cursor_hidden(false);
        }
#ifdef __ANDROID__
        else if (change.section == "controls")
            configure_screen_controls();
#endif
    };
    const auto open_menu = [&] {
        if (menu || !session_ptr || !emuenv.overlay_manager || !emuenv.renderer)
            return;
        menu = emuenv.overlay_manager->create<SettingsMenu>(profile.ini, settings.values, profile.name, settings.touches, profile.default_touches);
        emuenv.renderer->hide_pause_overlay = true;
        session_ptr->set_pause_reason(app::AppSessionPauseReason::Menu, true);
        emuenv.overlay_manager->attach_thread_input("settings_menu", menu);
#ifdef __ANDROID__
        android_settings_menu_open(true);
#endif
        LOG_INFO("Settings menu: open");
    };
    const auto close_menu = [&] {
        if (!menu)
            return;
        menu->request_close();
        menu->stop_input_processing(true);
        for (const auto &change : menu->take_changes())
            apply_change(change);
        emuenv.overlay_manager->remove<SettingsMenu>();
        menu.reset();
        session_ptr->set_pause_reason(app::AppSessionPauseReason::Menu, false);
        emuenv.renderer->hide_pause_overlay = false;
#ifdef __ANDROID__
        android_settings_menu_open(false);
#endif
        LOG_INFO("Settings menu: closed");
    };

    auto last_title = std::chrono::steady_clock::now();
    sdl_frontend::Hooks hooks;
    hooks.on_window = [&](SDL_Window *w) {
        window = w;
        if (!window)
            return;
        SDL_SetWindowTitle(window, ctx.window_name.c_str());
#ifdef __ANDROID__
        // the soft keyboard for the game's text dialogs, the log flush on pause (android.cpp)
        android_window_created(window);
        // the on-screen controls, now that there is a game for them to drive
        android_set_emuenv(&emuenv);
        configure_screen_controls();
#endif
        // the game's own icon, from the player's installed copy. Not on macOS: there a window has no
        // icon, and SDL would replace the Dock icon, the app bundle's (shaped as a macOS icon), with
        // this square picture
#ifndef __APPLE__
        const fs::path icon_path = emuenv.vita_fs_path / "ux0" / "app" / profile.title_id / "sce_sys" / "icon0.png";
        int icon_w = 0, icon_h = 0, channels = 0;
        if (stbi_uc *pixels = stbi_load(fs_utils::path_to_utf8(icon_path).c_str(), &icon_w, &icon_h, &channels, 4)) {
            if (SDL_Surface *icon = SDL_CreateSurfaceFrom(icon_w, icon_h, SDL_PIXELFORMAT_RGBA32, pixels, icon_w * 4)) {
                SDL_SetWindowIcon(window, icon);
                SDL_DestroySurface(icon);
            }
            stbi_image_free(pixels);
        }
#endif
#ifdef __ANDROID__
        // the whole screen, without the status and navigation bars (SDL's immersive mode)
        SDL_SetWindowFullscreen(window, true);
#else
        SDL_SetWindowSize(window, 960 * settings.window_scale, 544 * settings.window_scale);
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        if (settings.fullscreen && !hooks.hidden_window)
            SDL_SetWindowFullscreen(window, true);
#endif
        emuenv.touch.renderer_focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    };
    hooks.hidden_window = dump_elfs_seconds > 0;
    hooks.on_event = [&](const SDL_Event &event) {
        if (!window)
            return;
        // the mouse is the touchscreens, as in Vita3K's Qt window: left button front, right button rear
        auto &touch = emuenv.touch;
        const auto set_mouse = [&](float x, float y) {
            const float density = SDL_GetWindowPixelDensity(window); // window points -> drawable pixels
            touch.mouse_x = x * density;
            touch.mouse_y = y * density;
        };
        // a settings menu row being edited: the keyboard edits it (repeats too), and goes nowhere else
        if (menu && menu->text_editing()) {
            if (event.type == SDL_EVENT_TEXT_INPUT) {
                menu->text_input(event.text.text ? event.text.text : "");
                return;
            }
            if (event.type == SDL_EVENT_KEY_DOWN) {
                using TK = SettingsMenu::TextKey;
                switch (event.key.key) {
                case SDLK_BACKSPACE: menu->text_key(TK::Backspace); break;
                case SDLK_DELETE: menu->text_key(TK::Delete); break;
                case SDLK_LEFT: menu->text_key(TK::Left); break;
                case SDLK_RIGHT: menu->text_key(TK::Right); break;
                case SDLK_HOME: menu->text_key(TK::Home); break;
                case SDLK_END: menu->text_key(TK::End); break;
                case SDLK_RETURN:
                case SDLK_KP_ENTER: menu->text_key(TK::Enter); break;
                case SDLK_ESCAPE: menu->text_key(TK::Escape); break;
                case SDLK_F1: close_menu(); break;
                default: break;
                }
                return;
            }
        }
        // a Controls page binding waiting for its new key or button: the input goes to it and nowhere
        // else (releases still reach the keyboard state)
        if (menu && menu->binding_wait()) {
            switch (event.type) {
            case SDL_EVENT_KEY_DOWN:
                if (event.key.key == SDLK_F1)
                    close_menu();
                else if (!event.key.repeat)
                    menu->bind_key(event.key.scancode);
                return;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                if (event.button.which != SDL_TOUCH_MOUSEID && event.button.which != SDL_PEN_MOUSEID)
                    menu->bind_mouse(event.button.button);
                return;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                menu->bind_gamepad_button(event.gbutton.button);
                return;
            case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                if ((event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER || event.gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) && event.gaxis.value > 16384)
                    menu->bind_gamepad_trigger(event.gaxis.axis);
                break;
            default:
                break;
            }
        }
        switch (event.type) {
        case SDL_EVENT_KEY_DOWN:
            if (event.key.repeat)
                break;
            other_input_used();
#ifndef __ANDROID__
            // (on Android the system's Back and a device's extra buttons arrive as keys too: no keyboard)
            ctx.last_input = InputDevice::Keyboard;
#endif
            if (toggle_capture(key_from_scancode(event.key.scancode)))
                break;
            if (event.key.key == SDLK_ESCAPE && captured)
                capture_wanted = false; // and Esc goes on to the game, if it is bound (it closes an open menu only)
            if (event.key.key == SDLK_F1) {
                if (menu)
                    close_menu();
                else
                    open_menu();
            } else if (event.key.key == SDLK_ESCAPE && menu)
                close_menu();
#ifndef __ANDROID__
            else if (menu && (event.key.key == SDLK_RETURN || event.key.key == SDLK_KP_ENTER)
                && !(event.key.mod & SDL_KMOD_ALT))
                menu->on_button_pressed(overlay::pad_button::cross, false);
#endif
            else if (event.key.key == SDLK_F5) {
                // "that just happened": a line in the log to find the moment by in a bug report. A
                // binding on F5 still presses its button
                LOG_INFO("User mark {} at frame {} (F5)", ++user_marks, emuenv.display.frames_set.load());
                keyboard.key(emuenv, event.key.scancode, true);
            } else if (event.key.key == SDLK_F11 || (event.key.key == SDLK_RETURN && (event.key.mod & SDL_KMOD_ALT)))
                SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
            else if (event.key.key == SDLK_F12) {
                const auto t = std::time(nullptr);
                std::tm local{};
#ifdef _WIN32
                localtime_s(&local, &t);
#else
                localtime_r(&t, &local);
#endif
                save_frame(emuenv, ctx.data_dir / "screenshots" / fmt::format("{}_{:%Y-%m-%d_%H%M%S}.png", profile.id, local));
            } else
                keyboard.key(emuenv, event.key.scancode, true);
            break;
        case SDL_EVENT_KEY_UP:
            keyboard.key(emuenv, event.key.scancode, false);
            break;
        case SDL_EVENT_MOUSE_MOTION:
            if (event.motion.xrel != 0 || event.motion.yrel != 0)
                mouse_used();
            if (captured)
                mouse_camera.motion(event.motion.xrel, event.motion.yrel);
            else
                set_mouse(event.motion.x, event.motion.y);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            const bool physical_mouse = event.button.which != SDL_TOUCH_MOUSEID && event.button.which != SDL_PEN_MOUSEID;
            mouse_used();
#ifndef __ANDROID__
            // a real mouse's click, not one SDL made from a touch or a pen (a touchscreen PC, the Steam Deck)
            if (down && physical_mouse)
                ctx.last_input = InputDevice::Keyboard;
#endif
            if (down && physical_mouse && toggle_capture(mouse_button_code(event.button.button)))
                break;
            if (event.button.button == SDL_BUTTON_MIDDLE && down && mouse_camera_on() && !menu)
                capture_wanted = !capture_wanted;
            // the side buttons are bindings ([keyboard], [touch]: MouseX1, MouseX2)
            if (event.button.button == SDL_BUTTON_X1 || event.button.button == SDL_BUTTON_X2) {
                keyboard.mouse_button(emuenv, event.button.button, down);
                break;
            }
            // the captured cursor touches nothing: it is hidden, and stays where it was captured; its left
            // and right buttons are bindings then (MouseLeft, MouseRight)
            if (captured) {
                if (event.button.button == SDL_BUTTON_LEFT || event.button.button == SDL_BUTTON_RIGHT)
                    keyboard.mouse_button(emuenv, event.button.button, down);
                break;
            }
            set_mouse(event.button.x, event.button.y);
            if (event.button.button == SDL_BUTTON_LEFT)
                touch.mouse_button_left = down;
            else if (event.button.button == SDL_BUTTON_RIGHT)
                touch.mouse_button_right = down;
            break;
        }
        case SDL_EVENT_MOUSE_WHEEL:
            mouse_used();
            break;
        // a gamepad hides the cursor while the window has the focus (the cursor is the whole desktop's)
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            if (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) {
                other_input_used();
                ctx.last_input = InputDevice::Gamepad;
            }
            break;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            // past a quarter of the way, so a resting stick's drift doesn't count
            if (std::abs(event.gaxis.value) > 8192 && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS)) {
                other_input_used();
                ctx.last_input = InputDevice::Gamepad;
            }
            break;
        case SDL_EVENT_WINDOW_FOCUS_GAINED:
            touch.renderer_focused = true;
            break;
        case SDL_EVENT_WINDOW_FOCUS_LOST:
            keyboard.release_all(emuenv);
            touch.renderer_focused = false;
            touch.mouse_button_left = false;
            touch.mouse_button_right = false;
            break;
        default:
            break;
        }
    };
    const auto session_start = std::chrono::steady_clock::now();
    bool mipmaps_set = false;

    // A recompiled module that didn't load (built for another version, or needing a library this
    // platform's app lacks): on NativeCPU the game would stop at its first call into it with nothing on
    // the screen (utakata 0.2.0 on Android, 2026-11-25). Say so and quit instead.
    bool load_failure_reported = false;
    hooks.on_tick = [&] {
        if (!load_failure_reported && aot_native_cpu()) {
            if (const std::string failures = aot_load_failures(); !failures.empty()) {
                load_failure_reported = true;
                fail(fmt::format("The game's native code could not be loaded, so it can't run:\n\n{}\n"
#ifdef __ANDROID__
                                 "Build the game again: on a PC with --android, or from this app's setup after "
                                 "reinstalling the app. See {}/vita3k.log.",
#else
                                 "Build the game again: start it with --install. See {}/vita3k.log.",
#endif
                                 failures, profile.id));
                SDL_Event quit{};
                quit.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&quit);
            }
        }
        bool menu_toggle = false;
        {
            const std::lock_guard<std::mutex> lock(emuenv.ctrl.mutex);
            std::swap(menu_toggle, emuenv.ctrl.injected_menu_toggle);
        }
        if (menu_toggle) {
            if (menu)
                close_menu();
            else
                open_menu();
        }
        // keys the test driver pressed (v3kdrive key:<name>): down and up through SDL's queue, so they
        // reach on_event as the window's own would; and the gamepad inputs it holds (v3kdrive gpad:)
        std::vector<std::string> keys;
        std::optional<std::vector<std::string>> pad_inputs;
        {
            const std::lock_guard<std::mutex> lock(emuenv.ctrl.mutex);
            keys.swap(emuenv.ctrl.injected_keys);
            pad_inputs.swap(emuenv.ctrl.injected_gamepad);
        }
        if (pad_inputs)
            hold_test_gamepad(*pad_inputs);
        for (const auto &name : keys) {
            const SDL_Keycode key = SDL_GetKeyFromName(name.c_str());
            if (key == SDLK_UNKNOWN) {
                LOG_WARN("Input pipe: no key named '{}'", name);
                continue;
            }
            for (const bool down : { true, false }) {
                SDL_Event event{};
                event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
                event.key.windowID = window ? SDL_GetWindowID(window) : 0;
                event.key.key = key;
                event.key.scancode = SDL_GetScancodeFromKey(key, nullptr);
                event.key.down = down;
                SDL_PushEvent(&event);
            }
        }
        if (menu) {
            for (const auto &change : menu->take_changes())
                apply_change(change);
            if (menu->exit_requested()) {
                // the menu's Exit the game: quit as when the window is closed
                LOG_INFO("Settings menu: exit the game");
                close_menu();
                SDL_Event quit{};
                quit.type = SDL_EVENT_QUIT;
                SDL_PushEvent(&quit);
            } else if (menu->close_requested())
                close_menu();
        }
        {
            // Select (Back, View) held for a second on any gamepad: the menu, open or closed
            bool select = false;
            for (const auto &[_, controller] : emuenv.ctrl.controllers)
                if (SDL_Gamepad *pad = controller.controller.get(); pad && SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK))
                    select = true;
            const auto now = std::chrono::steady_clock::now();
            if (!select)
                select_held = false;
            else if (!select_held) {
                select_held = true;
                select_used = false;
                select_down = now;
            } else if (!select_used && now - select_down >= std::chrono::seconds(1)) {
                select_used = true;
                if (menu)
                    close_menu();
                else
                    open_menu();
            }
        }
        // [display] mipmaps (layout::mipmaps; false in a game without it): the renderer exists from
        // the first ticks, before the game creates its textures
        if (!mipmaps_set && emuenv.renderer) {
            mipmaps_set = true;
            const bool mipmaps = settings.values.get_bool("display.mipmaps");
            emuenv.renderer->generate_mips = mipmaps;
            emuenv.renderer->smooth_mip_sampling = mipmaps && profile.smooth_mip_sampling;
            LOG_INFO("generated mipmaps: {}{}", mipmaps ? "on" : "off",
                emuenv.renderer->smooth_mip_sampling ? " (full chains, clamped LOD bias, trilinear)" : "");
            // [debug] surface_sync: which scenes wait for the GPU
            emuenv.renderer->surface_sync_wait = settings.surface_sync < 3 ? settings.surface_sync : 0;
            LOG_INFO("surface sync: {}", std::array<const char *, 4>{ "exact", "targeted", "latent", "off" }[settings.surface_sync]);
        }
        if (dump_elfs_seconds > 0 && std::chrono::steady_clock::now() - session_start > std::chrono::duration<double>(dump_elfs_seconds)) {
            // --dump-elfs: the modules the game loaded so far are written; then quit
            if (profile.hooks.before_dump_quit)
                profile.hooks.before_dump_quit(ctx);
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
            dump_elfs_seconds = 0;
        }
        if (profile.hooks.on_tick)
            profile.hooks.on_tick(ctx);
        // [touch] bindings and the test driver's fingers
        touches.update(emuenv, keyboard);
        // screenshots the test driver asked for through the input pipe
        std::vector<std::string> shots;
        std::vector<std::pair<int, std::string>> records;
        std::vector<std::tuple<std::string, uint32_t, uint32_t>> memdumps;
        std::vector<std::pair<uint32_t, std::vector<uint8_t>>> memwrites;
        std::vector<std::string> texts;
        bool text_submit = false;
        {
            const std::lock_guard<std::mutex> lock(emuenv.ctrl.mutex);
            shots.swap(emuenv.ctrl.injected_screenshots);
            records.swap(emuenv.ctrl.injected_records);
            memdumps.swap(emuenv.ctrl.injected_memdumps);
            memwrites.swap(emuenv.ctrl.injected_memwrites);
            texts.swap(emuenv.ctrl.injected_texts);
            std::swap(text_submit, emuenv.ctrl.injected_text_submit);
        }
        // text the test driver types (tools/v3kdrive.py text:), into the game's open keyboard dialog
        for (const auto &text : texts) {
            if (!is_any_ime_active(emuenv)) {
                LOG_WARN("text: no keyboard dialog is open; '{}' dropped", text);
                continue;
            }
            {
                const std::lock_guard<std::mutex> lock(emuenv.ime.mutex);
                ime_commit_text(emuenv.ime, string_utils::utf8_to_utf16(text));
            }
            LOG_INFO("text: typed '{}'", text);
        }
        if (text_submit)
            LOG_INFO("textsubmit: {}", submit_current_ime(emuenv) ? "Enter pressed" : "no keyboard dialog is open");
        for (const auto &[address, bytes] : memwrites) {
            // guest memory for research (tools/v3kdrive.py memwrite:), when every byte is mapped
            const uint32_t size = static_cast<uint32_t>(bytes.size());
            if (size && is_valid_addr_range(emuenv.mem, address, address + size)) {
                std::memcpy(Ptr<uint8_t>(address).get(emuenv.mem), bytes.data(), size);
                LOG_INFO("memwrite: {} bytes at {:#x}", size, address);
            } else
                LOG_WARN("memwrite: {:#x}+{:#x} is not mapped", address, size);
        }
        for (const auto &[path, address, size] : memdumps) {
            // guest memory for research (tools/v3kdrive.py memdump:); pages that are not mapped read as 0
            std::vector<uint8_t> bytes(size);
            for (uint32_t i = 0; i < size;) {
                // up to the end of this 4 KB page
                const uint32_t n = std::min<uint32_t>(0x1000 - ((address + i) & 0xfff), size - i);
                if (is_valid_addr(emuenv.mem, address + i))
                    std::memcpy(&bytes[i], Ptr<uint8_t>(address + i).get(emuenv.mem), n);
                i += n;
            }
            fs::ofstream out(fs_utils::utf8_to_path(path), std::ios::binary);
            out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
            LOG_INFO("memdump: {:#x}+{:#x} -> {}", address, size, path);
        }
        for (const auto &path : shots)
            save_frame(emuenv, fs_utils::utf8_to_path(path));
        for (const auto &[count, dir] : records)
            record_frames(emuenv, fs_utils::utf8_to_path(dir), count);
        const bool ime = is_any_ime_active(emuenv);
        // the mouse camera: the cursor is captured while the player wants it and the game has the window
        // to itself (no settings menu, no text dialog)
        if (window && has_mouse_camera) {
            const bool focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
            const bool capture = mouse_camera_on() && capture_wanted && focused && !menu && !ime;
            if (capture != captured) {
                SDL_SetWindowRelativeMouseMode(window, capture);
                captured = capture;
                if (captured) {
                    // a finger the mouse held lifts
                    emuenv.touch.mouse_button_left = false;
                    emuenv.touch.mouse_button_right = false;
                } else {
                    mouse_camera.clear(emuenv);
                    // and the buttons the captured mouse held as bindings let go
                    keyboard.mouse_button(emuenv, SDL_BUTTON_LEFT, false);
                    keyboard.mouse_button(emuenv, SDL_BUTTON_RIGHT, false);
                }
                LOG_INFO("Mouse camera: cursor {}", captured ? "captured" : "released");
            }
            // motion the test driver sent through the input pipe, as if the captured mouse moved
            float dx = 0, dy = 0;
            {
                const std::lock_guard<std::mutex> lock(emuenv.ctrl.mutex);
                std::swap(dx, emuenv.ctrl.injected_mouse_dx);
                std::swap(dy, emuenv.ctrl.injected_mouse_dy);
            }
            if (mouse_camera_on())
                mouse_camera.motion(dx, dy);
            mouse_camera.update(emuenv, SDL_GetTicksNS());
        }
        // the idle cursor hides while the window has the focus
        if (window && !cursor_hidden && hide_cursor_on() && (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS)
            && std::chrono::steady_clock::now() - last_mouse_use >= std::chrono::seconds(3))
            set_cursor_hidden(true);
        // typed text reaches the game's keyboard dialogs (a player name) only while SDL's text
        // input is on; the session handles Enter, Backspace and Escape itself. A settings menu row
        // being edited takes typed text too (and on Android shows the on-screen keyboard)
        if (window) {
            const bool want_text = ime || (menu && menu->text_editing());
            if (want_text != SDL_TextInputActive(window)) {
                if (want_text)
                    SDL_StartTextInput(window);
                else
                    SDL_StopTextInput(window);
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (!window || now - last_title < std::chrono::milliseconds(500))
            return;
        last_title = now;
        // the same fields as Vita3K's window title, which tools/v3kdrive.py reads
        const auto &cc = emuenv.cfg.current_config;
        const auto size = emuenv.display.next_rendered_frame.image_size;
        const auto title = fmt::format("{} | {} | {} FPS ({} ms) | {}x{} | {}", ctx.window_name, cc.backend_renderer,
            emuenv.fps, emuenv.ms_per_frame, static_cast<int>(size.x * cc.resolution_multiplier),
            static_cast<int>(size.y * cc.resolution_multiplier), cc.screen_filter);
        SDL_SetWindowTitle(window, title.c_str());
    };

    app::AppSessionController session(emuenv);
    session_ptr = &session;
#ifdef __ANDROID__
    // the pause in the background (android.cpp) reaches the session until run() leaves, before it goes
    android_set_session(&session);
    struct SessionLink {
        ~SessionLink() { android_set_session(nullptr); }
    } session_link;
#endif
    const int exit_code = sdl_frontend::run(emuenv, session, AppLaunchRequest{ .app_path = profile.title_id }, hooks);

#ifdef _WIN32
    CoUninitialize();
#endif
    return exit_code;
}

} // namespace game_frontend
