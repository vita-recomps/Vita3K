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

#include "profile.h"

#include "../archive.h"
#include "input.h"
#include "settings.h"

#include <app/functions.h>
#include <app/session_controller.h>
#include <config/state.h>
#include <config/version.h>
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
#include <packages/pkg.h>
#include <renderer/state.h>
#include <sdl-frontend/session.h>
#include <touch/state.h>
#include <util/log.h>
#include <util/net_utils.h>
#include <util/string_utils.h>
#include <util/sysinfo.h>

#include <rif2zrif.h>

#ifdef _WIN32
#include <combaseapi.h>
#endif

#include <SDL3/SDL.h>
#include <stb_image.h>
#include <stb_image_write.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace game_frontend {

namespace {

void set_env(const char *name, const std::string &value) {
#ifdef _WIN32
    _putenv_s(name, value.c_str());
#else
    setenv(name, value.c_str(), 1);
#endif
}

// SSD_AOT (the fork's list of recompiled modules) from the DLLs in `dir`, unless it is set already
std::string select_recompiled_modules(const fs::path &dir) {
    if (const char *const env = std::getenv("SSD_AOT"))
        return env;
    std::string list;
    if (fs::is_directory(dir)) {
        for (const auto &entry : fs::directory_iterator(dir)) {
            const auto name = entry.path().filename().string();
            if (name.starts_with("ssd_aot_") && entry.path().extension() == ".dll")
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

// a pkg's licence as a zRIF: from --zrif, from a licence file (work.bin, .rif), or, for a patch or
// a game installed before, from the licence already in the Vita file system
std::string pkg_zrif(const fs::path &pkg, const std::string &zrif, const std::string &license, const fs::path &vita_fs) {
    if (!zrif.empty())
        return zrif;
    if (!license.empty()) {
        fs::ifstream file(fs_utils::utf8_to_path(license), std::ios::binary | std::ios::ate);
        if (!file) {
            LOG_ERROR("Licence {} could not be read", license);
            return {};
        }
        return rif2zrif(file);
    }
    return find_pkg_zrif(pkg, vita_fs);
}

struct InstallItem {
    std::string path;
    std::string license; // --license after it: work.bin or .rif, for a pkg
    std::string zrif; // --zrif after it
};

} // namespace

int run(Profile &profile, int argc, char *argv[]) {
    // command line:
    //   --install-firmware <PUP>     install a firmware update file into the Vita file system
    //   --install <zip|vpk|folder|pkg> [--license <work.bin|rif> | --zrif <zRIF>]
    //                                install a game dump (app, patch, DLC) into it; a pkg needs its
    //                                licence, except a patch or add-on whose game's licence is there
    //   --dump-elfs <seconds>        run the game that long, writing every module as loaded to
    //                                <id>/elfdumps/ (the input for recompiling), then quit
    //   --vita-fs <folder>           the Vita file system to use instead of the settings file's
    //   --write-ini <file>           write the default settings file there and quit
    //   --player <1-4>               (games with Ad Hoc Play) a copy of the game for another player,
    //                                with its own Vita user (saves), config and log (<id>/player<N>/)
    //   and the game's own options (its Hooks::parse_arg)
    Context ctx;
    for (int i = 1; i < argc; i++)
        ctx.args.emplace_back(argv[i]);
    const std::string exe = profile.id + ".exe";

    std::vector<std::string> install_firmware;
    std::vector<InstallItem> install_content;
    double dump_elfs_seconds = 0;
    std::string vita_fs_override;
    std::vector<std::string> unknown_args;
    for (size_t i = 0; i < ctx.args.size(); i++) {
        const std::string &arg = ctx.args[i];
        const bool has_value = i + 1 < ctx.args.size();
        if (arg == "--install-firmware" && has_value)
            install_firmware.push_back(ctx.args[++i]);
        else if (arg == "--install" && has_value)
            install_content.push_back({ ctx.args[++i] });
        else if ((arg == "--license" || arg == "--zrif") && has_value && !install_content.empty())
            (arg == "--license" ? install_content.back().license : install_content.back().zrif) = ctx.args[++i];
        else if (arg == "--dump-elfs" && has_value)
            dump_elfs_seconds = std::atof(ctx.args[++i].c_str());
        else if (arg == "--vita-fs" && has_value)
            vita_fs_override = ctx.args[++i];
        else if (arg == "--write-ini" && has_value) {
            // the default settings file, for packaging scripts; nothing else runs
            const fs::path path = fs_utils::utf8_to_path(ctx.args[++i]);
            fs::ofstream(path) << default_ini(profile.ini);
            return fs::exists(path) ? 0 : 1;
        } else if (arg == "--player" && has_value && profile.multiplayer)
            ctx.player = std::clamp(std::atoi(ctx.args[++i].c_str()), 1, net_utils::LOCAL_ADHOC_MAX);
        else if (!profile.hooks.parse_arg || !profile.hooks.parse_arg(ctx, ctx.args, i))
            unknown_args.push_back(arg);
    }
    // player N is the Vita user N-1 (00 for player 1, the user every install has)
    ctx.user_id = fmt::format("{:02}", ctx.player - 1);
    ctx.window_name = ctx.player == 1 ? profile.name : fmt::format("{} (player {})", profile.name, ctx.player);
    const auto fail = [&](const std::string &message) {
        LOG_ERROR("{}", message);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, ctx.window_name.c_str(), message.c_str(), nullptr);
    };

    Root root_paths;
    app::init_paths(root_paths);
    ctx.exe_dir = root_paths.get_static_assets_path();
    // our own config and log, apart from a Vita3K.exe in the same folder; the shader cache is shared.
    // Players 2 and up have theirs in a folder of their own.
    ctx.data_dir = ctx.player == 1 ? ctx.exe_dir / profile.id / "" : ctx.exe_dir / profile.id / fmt::format("player{}", ctx.player) / "";
    fs::create_directories(ctx.data_dir);
    root_paths.set_config_path(ctx.data_dir);
    root_paths.set_log_path(ctx.data_dir);
    root_paths.set_shared_path(ctx.data_dir);

    Settings settings = load_settings(ctx.exe_dir, profile.id, profile.ini, profile.default_touches, ctx.player);
    if (!vita_fs_override.empty())
        settings.vita_fs = fs_utils::utf8_to_path(vita_fs_override) / "";
    root_paths.set_vita_fs_path(settings.vita_fs);
    ctx.settings = &settings;

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

    if (profile.hooks.before_init) {
        if (const auto exit_code = profile.hooks.before_init(ctx))
            return *exit_code;
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
    // a game's configure hook can change them
    cfg.backend_renderer = "Vulkan";
    cfg.high_accuracy = true;
    cfg.disable_surface_sync = false;
    cfg.memory_mapping = "double-buffer";
    // a pipeline seen for the first time is compiled in the background (its draws are skipped
    // meanwhile) and kept in the pipeline cache next to the executable; no on-screen notice
    cfg.async_pipeline_compilation = true;
    cfg.show_compile_shaders = false;
    cfg.validation_layer = settings.validation_layer;
    // the player's choices
    cfg.resolution_multiplier = settings.scale;
    cfg.v_sync = settings.vsync;
    cfg.screen_filter = settings.filter;
    cfg.audio_volume = settings.volume;
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
    if (!app::init(emuenv, cfg, root_paths)) {
        fail(fmt::format("The emulated environment could not be initialised. See {}/vita3k.log.", profile.id));
        return 1;
    }
    if (emuenv.cfg.controller_binds.size() != 15 || emuenv.cfg.controller_axis_binds.size() != 6)
        app::reset_controller_binding(emuenv);
    {
        std::vector<std::string> binding_warnings;
        apply_bindings(emuenv.cfg, settings.keyboard, settings.gamepad, binding_warnings);
        for (const auto &line : binding_warnings)
            LOG_WARN("{}.ini: {}", profile.id, line);
    }

    // SSD_DUMP_ELFS: write every module as loaded (decrypted, relocated) to <id>/elfdumps/, the
    // input for recompiling the firmware modules the game loads
    if (std::getenv("SSD_DUMP_ELFS") || dump_elfs_seconds > 0)
        emuenv.kernel.debugger.dump_elfs = true;

    init_libraries(emuenv);

    if (!install_firmware.empty() || !install_content.empty()) {
        bool ok = true;
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
            if (fs::is_directory(path)) {
                ok = install_contents(emuenv, path) > 0 && ok;
            } else if (string_utils::tolower(path.extension().string()) == ".pkg") {
                std::string zrif = pkg_zrif(path, item.zrif, item.license, emuenv.vita_fs_path);
                if (zrif.empty()) {
                    LOG_ERROR("{}: no licence (give --license <work.bin> or --zrif after it)", item.path);
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
                const auto installed = install_archive(emuenv, path);
                ok = !installed.empty() && std::all_of(installed.begin(), installed.end(), [](const ContentInfo &c) { return c.state; }) && ok;
            }
        }
        LOG_INFO("Installation {}", ok ? "done" : "failed");
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
    if (ctx.player > 1) {
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
        fail(fmt::format("{} ({}) is not installed in {}.\nSet [paths] vita_fs in {}.ini.",
            profile.name, profile.title_id, emuenv.vita_fs_path.string(), profile.id));
        return 1;
    }

    if (profile.hooks.before_run)
        profile.hooks.before_run(ctx);

    // window: size and fullscreen from the settings, F11 or Alt+Enter switch, frame rate in the title
    SDL_Window *window = nullptr;
    Keyboard keyboard;
    Touches touches;
    {
        std::vector<std::string> touch_warnings;
        touches.configure(settings.touches, touch_warnings);
        for (const auto &line : touch_warnings)
            LOG_WARN("{}.ini: {}", profile.id, line);
    }
    auto last_title = std::chrono::steady_clock::now();
    sdl_frontend::Hooks hooks;
    hooks.on_window = [&](SDL_Window *w) {
        window = w;
        if (!window)
            return;
        SDL_SetWindowTitle(window, ctx.window_name.c_str());
        // the game's own icon, from the player's installed copy
        const fs::path icon_path = emuenv.vita_fs_path / "ux0" / "app" / profile.title_id / "sce_sys" / "icon0.png";
        int icon_w = 0, icon_h = 0, channels = 0;
        if (stbi_uc *pixels = stbi_load(fs_utils::path_to_utf8(icon_path).c_str(), &icon_w, &icon_h, &channels, 4)) {
            if (SDL_Surface *icon = SDL_CreateSurfaceFrom(icon_w, icon_h, SDL_PIXELFORMAT_RGBA32, pixels, icon_w * 4)) {
                SDL_SetWindowIcon(window, icon);
                SDL_DestroySurface(icon);
            }
            stbi_image_free(pixels);
        }
        SDL_SetWindowSize(window, 960 * settings.window_scale, 544 * settings.window_scale);
        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        if (settings.fullscreen)
            SDL_SetWindowFullscreen(window, true);
        emuenv.touch.renderer_focused = (SDL_GetWindowFlags(window) & SDL_WINDOW_INPUT_FOCUS) != 0;
    };
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
        switch (event.type) {
        case SDL_EVENT_KEY_DOWN:
            if (event.key.repeat)
                break;
            if (event.key.key == SDLK_F11 || (event.key.key == SDLK_RETURN && (event.key.mod & SDL_KMOD_ALT)))
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
            set_mouse(event.motion.x, event.motion.y);
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP: {
            set_mouse(event.button.x, event.button.y);
            const bool down = event.type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            if (event.button.button == SDL_BUTTON_LEFT)
                touch.mouse_button_left = down;
            else if (event.button.button == SDL_BUTTON_RIGHT)
                touch.mouse_button_right = down;
            break;
        }
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
    hooks.on_tick = [&] {
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
        {
            const std::lock_guard<std::mutex> lock(emuenv.ctrl.mutex);
            shots.swap(emuenv.ctrl.injected_screenshots);
            records.swap(emuenv.ctrl.injected_records);
        }
        for (const auto &path : shots)
            save_frame(emuenv, fs_utils::utf8_to_path(path));
        for (const auto &[count, dir] : records)
            record_frames(emuenv, fs_utils::utf8_to_path(dir), count);
        // typed text reaches the game's keyboard dialogs (a player name) only while SDL's text
        // input is on; the session handles Enter, Backspace and Escape itself
        if (window) {
            const bool ime = is_any_ime_active(emuenv);
            if (ime != SDL_TextInputActive(window)) {
                if (ime)
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
    const int exit_code = sdl_frontend::run(emuenv, session, AppLaunchRequest{ .app_path = profile.title_id }, hooks);

#ifdef _WIN32
    CoUninitialize();
#endif
    return exit_code;
}

} // namespace game_frontend
