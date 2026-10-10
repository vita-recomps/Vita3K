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

// ss-delta.exe: Soul Sacrifice Delta (PCSA00152) on the SDL frontend, without the Qt interface.
// Settings that the game needs are fixed here; the player's choices come from ss-delta.ini next
// to the executable. The recompiled modules (ssd_aot_<module>.dll, generated locally from the
// player's own dump) are loaded from the same folder.

#include "settings.h"

#include <app/functions.h>
#include <app/session_controller.h>
#include <config/state.h>
#include <config/version.h>
#include <display/state.h>
#include <emuenv/state.h>
#include <ime/functions.h>
#include <modules/module_parent.h>
#include <sdl-frontend/session.h>
#include <touch/state.h>
#include <util/log.h>
#include <util/string_utils.h>
#include <util/sysinfo.h>

#ifdef _WIN32
#include <combaseapi.h>
#endif

#include <SDL3/SDL.h>
// provides WinMain for the windowed executable, which calls main below
#include <SDL3/SDL_main.h>

#include <chrono>
#include <cstdlib>
#include <string>

namespace {

constexpr const char *TITLE_ID = "PCSA00152";
constexpr const char *WINDOW_NAME = "Soul Sacrifice Delta";

void fail(const std::string &message) {
    LOG_ERROR("{}", message);
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, WINDOW_NAME, message.c_str(), nullptr);
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
    if (!list.empty()) {
#ifdef _WIN32
        _putenv_s("SSD_AOT", list.c_str());
#else
        setenv("SSD_AOT", list.c_str(), 1);
#endif
    }
    return list;
}

} // namespace

int main(int argc, char *argv[]) {
    Root root_paths;
    app::init_paths(root_paths);
    const fs::path exe_dir = root_paths.get_static_assets_path();
    // our own config and log, apart from a Vita3K.exe in the same folder; the shader cache is shared
    const fs::path data_dir = exe_dir / "ss-delta" / "";
    fs::create_directories(data_dir);
    root_paths.set_config_path(data_dir);
    root_paths.set_log_path(data_dir);
    root_paths.set_shared_path(data_dir);

    const ss_delta::Settings settings = ss_delta::load_settings(exe_dir);
    root_paths.set_vita_fs_path(settings.vita_fs);

    if (logging::init(root_paths, false) != Success)
        return 1;
    logging::set_level(static_cast<spdlog::level::level_enum>(settings.log_level));
    LOG_INFO("ss-delta on {}", window_title);
    LOG_INFO("CPU: {}", util::get_system_info());
    for (const auto &line : settings.warnings)
        LOG_WARN("ss-delta.ini: ignored \"{}\"", line);

    if (settings.use_recompiled) {
        const std::string modules = select_recompiled_modules(settings.recompiled);
        LOG_INFO_IF(!modules.empty(), "Recompiled modules: {}", modules);
        LOG_WARN_IF(modules.empty(), "No recompiled modules in {}: everything runs on the JIT", settings.recompiled);
    }

    Config cfg{};
    cfg.frontend = Frontend::sdl;
    cfg.run_app_path = TITLE_ID;
    cfg.config_path = data_dir / "config.yml";
    cfg.set_vita_fs_path(settings.vita_fs);
    cfg.log_level = settings.log_level;
    cfg.discord_rich_presence = false;
    cfg.check_for_updates = false;
    // what the game needs (docs/decisions.md D4 update 4 in ss-delta): Vulkan, surface sync and
    // high accuracy for its fog, haze and smoke, double-buffered memory mapping
    cfg.backend_renderer = "Vulkan";
    cfg.high_accuracy = true;
    cfg.disable_surface_sync = false;
    cfg.memory_mapping = "double-buffer";
    cfg.validation_layer = settings.validation_layer;
    // the player's choices
    cfg.resolution_multiplier = settings.scale;
    cfg.v_sync = settings.vsync;
    cfg.screen_filter = settings.filter;
    cfg.audio_volume = settings.volume;

    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_ENHANCED_REPORTS, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_SWITCH, "1");
    SDL_SetHint(SDL_HINT_JOYSTICK_HIDAPI_JOY_CONS, "1");

#ifdef _WIN32
    LOG_ERROR_IF(CoInitializeEx(nullptr, COINIT_MULTITHREADED) == S_FALSE, "Failed to initialize COM Library");
#endif

    EmuEnvState emuenv;
    if (!app::init(emuenv, cfg, root_paths)) {
        fail("The emulated environment could not be initialised. See ss-delta/vita3k.log.");
        return 1;
    }
    if (emuenv.cfg.controller_binds.size() != 15 || emuenv.cfg.controller_axis_binds.size() != 6)
        app::reset_controller_binding(emuenv);

    init_libraries(emuenv);
    if (!app::init_apps_list(emuenv)) {
        fail("The list of installed apps could not be read. See ss-delta/vita3k.log.");
        return 1;
    }
    app::load_users(emuenv);

    if (!fs::exists(emuenv.vita_fs_path / "ux0" / "app" / TITLE_ID / "eboot.bin")) {
        fail(fmt::format("Soul Sacrifice Delta ({}) is not installed in {}.\nSet [paths] vita_fs in ss-delta.ini.",
            TITLE_ID, emuenv.vita_fs_path.string()));
        return 1;
    }

    // window: size and fullscreen from the settings, F11 or Alt+Enter switch, frame rate in the title
    SDL_Window *window = nullptr;
    auto last_title = std::chrono::steady_clock::now();
    sdl_frontend::Hooks hooks;
    hooks.on_window = [&](SDL_Window *w) {
        window = w;
        if (!window)
            return;
        SDL_SetWindowTitle(window, WINDOW_NAME);
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
            if (!event.key.repeat && (event.key.key == SDLK_F11 || (event.key.key == SDLK_RETURN && (event.key.mod & SDL_KMOD_ALT))))
                SDL_SetWindowFullscreen(window, !(SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN));
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
            touch.renderer_focused = false;
            touch.mouse_button_left = false;
            touch.mouse_button_right = false;
            break;
        default:
            break;
        }
    };
    hooks.on_tick = [&] {
        // typed text reaches the game's keyboard dialogs (the player name) only while SDL's text
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
        const auto title = fmt::format("{} | {} | {} FPS ({} ms) | {}x{} | {}", WINDOW_NAME, cc.backend_renderer,
            emuenv.fps, emuenv.ms_per_frame, static_cast<int>(size.x * cc.resolution_multiplier),
            static_cast<int>(size.y * cc.resolution_multiplier), cc.screen_filter);
        SDL_SetWindowTitle(window, title.c_str());
    };

    app::AppSessionController session(emuenv);
    const int exit_code = sdl_frontend::run(emuenv, session, AppLaunchRequest{ .app_path = TITLE_ID }, hooks);

#ifdef _WIN32
    CoUninitialize();
#endif
    return exit_code;
}
