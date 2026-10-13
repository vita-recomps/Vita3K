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

#include "../archive.h"
#include "dlc.h"
#include "input.h"
#include "saves.h"
#include "settings.h"

#include <app/functions.h>
#include <app/session_controller.h>
#include <config/state.h>
#include <config/version.h>
#include <ctrl/state.h>
#include <display/state.h>
#include <emuenv/state.h>
#include <packages/functions.h>
#include <kernel/state.h>
#include <mem/functions.h>
#include <mem/ptr.h>
#include <renderer/state.h>
#include <ime/functions.h>
#include <gxm/state.h>
#include <gxm/types.h>
#include <io/functions.h>
#include <io/state.h>
#include <modules/module_parent.h>
#include <sdl-frontend/session.h>
#include <touch/state.h>
#include <util/log.h>
#include <util/net_utils.h>
#include <util/string_utils.h>
#include <util/sysinfo.h>

#ifdef _WIN32
#include <combaseapi.h>
#endif

#include <SDL3/SDL.h>
#include <stb_image.h>
#include <stb_image_write.h>
// provides WinMain for the windowed executable, which calls main below
#include <SDL3/SDL_main.h>

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <algorithm>
#include <string>
#include <vector>

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

// The game reads its engine settings from app0:resource/boot_config.ini, "Key<tab>Value" lines in
// two blocks (development, then retail). Serves a copy, written to `dir`, in which each key of
// `values` has the given value everywhere it is set; the installed file stays as it is.
void override_boot_config(EmuEnvState &emuenv, const fs::path &dir, const std::map<std::string, std::string> &values) {
    const fs::path installed = emuenv.vita_fs_path / "ux0" / "app" / TITLE_ID / "resource" / "boot_config.ini";
    fs::ifstream in(installed, std::ios::binary);
    if (!in) {
        LOG_WARN("boot_config.ini not found at {}; the game's own settings stay", installed);
        return;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string out;
    std::set<std::string> found;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        end = end == std::string::npos ? text.size() : end + 1;
        std::string line = text.substr(pos, end - pos);
        const auto tab = line.find('\t');
        if (tab != std::string::npos) {
            const auto it = values.find(line.substr(0, tab));
            if (it != values.end()) {
                const bool crlf = line.ends_with("\r\n");
                line = it->first + "\t" + it->second + (crlf ? "\r\n" : "\n");
                found.insert(it->first);
            }
        }
        out += line;
        pos = end;
    }
    for (const auto &[key, value] : values) {
        if (!found.contains(key))
            out += key + "\t" + value + "\r\n";
        LOG_INFO("boot_config.ini: {} {}", key, value);
    }
    fs::create_directories(dir);
    const fs::path copy = dir / "boot_config.ini";
    fs::ofstream(copy, std::ios::binary) << out;
    emuenv.io.read_overrides[read_override_key(installed)] = copy;
}

// With the 3D at 960x544 (full_3d), the game samples its depth buffer through vertex shaders that
// scale the coordinates by 720/736 and 408/416: right for the 720x408 mode, whose depth texture is
// padded to whole 32-pixel tiles (1472x832 samples), 2% short at 960x544, which needs no padding.
// Depth of field and haze then use the depth beside each pixel (bones in the hub blurred). Both
// values are in the shaders' literal table, a list of {u32 register, f32 value}, as consecutive
// entries; they are set to 1.0 there.
void unscale_depth_uvs(SceGxmProgram &program) {
    constexpr uint32_t x_scale = 0x3f7a6f4e; // 720 / 736
    constexpr uint32_t y_scale = 0x3f7b13b2; // 408 / 416
    constexpr uint32_t one = 0x3f800000;
    uint8_t *const bytes = reinterpret_cast<uint8_t *>(&program);
    int replaced = 0;
    for (uint32_t i = 0; i + 12 <= program.size; i += 4) {
        uint32_t x, y;
        std::memcpy(&x, bytes + i, 4);
        std::memcpy(&y, bytes + i + 8, 4);
        if (x == x_scale && y == y_scale) {
            std::memcpy(bytes + i, &one, 4);
            std::memcpy(bytes + i + 8, &one, 4);
            replaced++;
        }
    }
    LOG_INFO_IF(replaced, "full_3d: depth UV scale set to 1.0 in a {} shader", program.is_vertex() ? "vertex" : "fragment");
}

// The frame rate (docs/03-plan.md, 60 fps): the game paces its main loop with a frame timer
// created for 30 fps (0x812ab888: movs r0, #30), whose period in microseconds is the first u64 of
// the timer object (*(*0x81c957cc)), and flips every 2nd vblank (threshold at 0x81d3a630). Both
// are data, so they are changed in guest memory once the game has set them up. Game logic steps
// by the measured frame time, so it keeps its speed at 60 fps, but the loading screen does not
// finish with frames shorter than about 1/45 s: its "roading_loop" animation never gets told to
// stop. So loading screens run at 30 fps: the NowLoadingTask singleton (*0x81c9603c) shows one
// while its state (+0xa4) is 2, 3 or 5 (opening, "roading_loop", "roading_end"; 0x812b58ec). After
// a quest it plays the loop backwards (6) and rests in 4 until reset, so those stay at 60.
// Called every frame; returns the rate it set (0: not yet).
int update_frame_rate(EmuEnvState &emuenv, int fps, int current) {
    auto &mem = emuenv.mem;
    const auto u32 = [&](Address a) -> uint32_t * { return is_valid_addr(mem, a) ? Ptr<uint32_t>(a).get(mem) : nullptr; };
    uint32_t *const holder = u32(0x81c957cc);
    if (!holder || !*holder)
        return 0;
    uint32_t *const timer = u32(*holder);
    uint32_t *const flip = u32(0x81d3a630);
    if (!timer || !*timer || !is_valid_addr(mem, *timer + 7) || !flip)
        return 0;
    uint64_t *const period = Ptr<uint64_t>(*timer).get(mem);
    const uint32_t *const loading_task = u32(0x81c9603c);
    const uint32_t *const loading_state = loading_task && *loading_task ? u32(*loading_task + 0xa4) : nullptr;
    const uint32_t state = loading_state ? *loading_state : 0;
    const int want = state == 2 || state == 3 || state == 5 ? 30 : fps;
    if (want == current && *period == 1000000u / want)
        return current;
    if ((*period != 1000000u / 30 && *period != 1000000u / fps) || (*flip != 2 && *flip != 60u / fps)) {
        LOG_WARN("frame_rate: unexpected frame timer {} us / flip threshold {}; left alone", *period, *flip);
        return -1;
    }
    *period = 1000000 / want;
    *flip = 60 / want;
    LOG_INFO("frame_rate: {} fps{} (loading state {}, frame timer {} us, flip every {} vblank)", want, want != fps ? " for a loading screen" : "", state, *period, *flip);
    return want;
}

} // namespace

int main(int argc, char *argv[]) {
    // command line (tools/install-game.ps1 in ss-delta uses it):
    //   --install-firmware <PUP>   install a firmware update file into the Vita file system
    //   --install <zip|vpk|folder> install a game dump (app, patch, DLC) into it
    //   --dump-elfs <seconds>      run the game that long, writing every module as loaded to
    //                              ss-delta/elfdumps/ (the input for recompiling), then quit
    //   --vita-fs <folder>         the Vita file system to use instead of ss-delta.ini's
    //   --import-save <folder|zip> copy a save (decrypted, e.g. from a Vita) in, keeping the old one
    //   --export-save <folder>     copy a save out
    //   --slot <1-3>               the game's save slot for the two above (default 1)
    //   --quiet                    no message box for the two above (the result is in the log)
    //   --player <1-4>             a copy of the game for another player of Ad Hoc Play, with its
    //                              own Vita user (saves), config and log (ss-delta/player<N>/)
    std::vector<std::string> install_firmware, install_content;
    double dump_elfs_seconds = 0;
    std::string vita_fs_override, import_save_from, export_save_to;
    bool quiet = false;
    int save_slot = 1;
    int player = 1;
    for (int i = 1; i < argc; i++) {
        const std::string arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--install-firmware" && has_value)
            install_firmware.push_back(argv[++i]);
        else if (arg == "--install" && has_value)
            install_content.push_back(argv[++i]);
        else if (arg == "--dump-elfs" && has_value)
            dump_elfs_seconds = std::atof(argv[++i]);
        else if (arg == "--vita-fs" && has_value)
            vita_fs_override = argv[++i];
        else if (arg == "--import-save" && has_value)
            import_save_from = argv[++i];
        else if (arg == "--export-save" && has_value)
            export_save_to = argv[++i];
        else if (arg == "--slot" && has_value)
            save_slot = std::atoi(argv[++i]);
        else if (arg == "--quiet")
            quiet = true;
        else if (arg == "--player" && has_value)
            player = std::clamp(std::atoi(argv[++i]), 1, net_utils::LOCAL_ADHOC_MAX);
    }
    // player N is the Vita user N-1 (00 for player 1, the user every install has)
    const std::string user_id = fmt::format("{:02}", player - 1);
    const std::string window_name = player == 1 ? WINDOW_NAME : fmt::format("{} (player {})", WINDOW_NAME, player);

    Root root_paths;
    app::init_paths(root_paths);
    const fs::path exe_dir = root_paths.get_static_assets_path();
    // our own config and log, apart from a Vita3K.exe in the same folder; the shader cache is shared.
    // Players 2 and up have theirs in a folder of their own.
    const fs::path data_dir = player == 1 ? exe_dir / "ss-delta" / "" : exe_dir / "ss-delta" / fmt::format("player{}", player) / "";
    fs::create_directories(data_dir);
    root_paths.set_config_path(data_dir);
    root_paths.set_log_path(data_dir);
    root_paths.set_shared_path(data_dir);

    ss_delta::Settings settings = ss_delta::load_settings(exe_dir, player);
    if (!vita_fs_override.empty())
        settings.vita_fs = fs_utils::utf8_to_path(vita_fs_override) / "";
    root_paths.set_vita_fs_path(settings.vita_fs);

    if (logging::init(root_paths, false) != Success)
        return 1;
    logging::set_level(static_cast<spdlog::level::level_enum>(settings.log_level));
    LOG_INFO("ss-delta on {}", window_title);
    LOG_INFO_IF(player > 1, "Player {} (Vita user {})", player, user_id);
    LOG_INFO("CPU: {}", util::get_system_info());
    for (const auto &line : settings.warnings)
        LOG_WARN("ss-delta.ini: ignored \"{}\"", line);

    ss_delta::set_save_user(user_id);
    if (!import_save_from.empty() || !export_save_to.empty()) {
        std::string message;
        const bool ok = !import_save_from.empty()
            ? ss_delta::import_save(settings.vita_fs, fs_utils::utf8_to_path(import_save_from), save_slot, data_dir / "save-backups", message)
            : ss_delta::export_save(settings.vita_fs, fs_utils::utf8_to_path(export_save_to), save_slot, message);
        if (ok)
            LOG_INFO("Save: {}", message);
        else
            LOG_ERROR("Save: {}", message);
        if (!quiet)
            SDL_ShowSimpleMessageBox(ok ? SDL_MESSAGEBOX_INFORMATION : SDL_MESSAGEBOX_ERROR, window_name.c_str(),
                ("Save: " + message).c_str(), nullptr);
        return ok ? 0 : 1;
    }

    if (settings.use_recompiled) {
        const std::string modules = select_recompiled_modules(settings.recompiled);
        LOG_INFO_IF(!modules.empty(), "Recompiled modules: {}", modules);
        LOG_WARN_IF(modules.empty(), "No recompiled modules in {}: everything runs on the JIT", settings.recompiled);
        // the native CPU (no JIT) unless [recompiled] jit = true; SSD_AOT_NATIVE set beforehand wins
        if (!modules.empty() && !settings.jit && !std::getenv("SSD_AOT_NATIVE")) {
#ifdef _WIN32
            _putenv_s("SSD_AOT_NATIVE", "1");
#else
            setenv("SSD_AOT_NATIVE", "1", 1);
#endif
        }
        LOG_INFO("CPU: {}", !modules.empty() && std::getenv("SSD_AOT_NATIVE") && std::string(std::getenv("SSD_AOT_NATIVE")) == "1"
                ? "native code only (NativeCPU)" : "JIT with native code");
    }

    Config cfg{};
    cfg.frontend = Frontend::sdl;
    cfg.run_app_path = TITLE_ID;
    cfg.config_path = data_dir / "config.yml";
    cfg.set_vita_fs_path(settings.vita_fs);
    cfg.log_level = settings.log_level;
    cfg.discord_rich_presence = false;
    // the game's online services are gone: no network at all, as on a Vita in flight mode
    cfg.offline = true;
    // Ad Hoc Play: copies on this PC talk over 127.0.0.<player>; on a network, through an adapter
    // (Vita3K's adhoc-addr). Players 2 and up only read the shader cache player 1 keeps.
    net_utils::set_local_adhoc(settings.lan ? 0 : player);
    cfg.adhoc_addr = settings.adapter;
    cfg.shader_cache_read_only = player > 1;
    cfg.check_for_updates = false;
    // what the game needs (docs/decisions.md D4 update 4 in ss-delta): Vulkan, surface sync and
    // high accuracy for its fog, haze and smoke, double-buffered memory mapping
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

    // windows without an icon of their own get the executable's (ss-delta.rc)
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON, "101");
    SDL_SetHint(SDL_HINT_WINDOWS_INTRESOURCE_ICON_SMALL, "101");
    SDL_SetHint(SDL_HINT_JOYSTICK_THREAD, "1");
    // [multiplayer] gamepad: one gamepad per copy of the game, read also while its window is in
    // the background
    if (settings.player_gamepad > 0)
        SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
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
    emuenv.ctrl.only_gamepad = settings.player_gamepad;
    if (emuenv.cfg.controller_binds.size() != 15 || emuenv.cfg.controller_axis_binds.size() != 6)
        app::reset_controller_binding(emuenv);
    {
        std::vector<std::string> binding_warnings;
        ss_delta::apply_bindings(emuenv.cfg, settings.keyboard, settings.gamepad, binding_warnings);
        for (const auto &line : binding_warnings)
            LOG_WARN("ss-delta.ini: {}", line);
    }

    // SSD_DUMP_ELFS: write every module as loaded (decrypted, relocated) to ss-delta/elfdumps/, the
    // input for recompiling the firmware modules the game loads (docs/05-recompiler.md in ss-delta)
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
        for (const auto &content : install_content) {
            const fs::path path = fs_utils::utf8_to_path(content);
            LOG_INFO("Installing {}", content);
            if (fs::is_directory(path)) {
                ok = install_contents(emuenv, path) > 0 && ok;
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
        fail("The list of installed apps could not be read. See ss-delta/vita3k.log.");
        return 1;
    }
    app::load_users(emuenv);
    if (player > 1) {
        // users 01, 02 ... in order, so that player N's is N-1
        for (int p = 2; p <= player; p++) {
            const std::string id = fmt::format("{:02}", p - 1);
            if (!emuenv.app.user_list.users.contains(id) && app::create_user(emuenv, fmt::format("Player {}", p)) != id)
                LOG_WARN("Vita user {} could not be created for player {}", id, p);
        }
        if (!emuenv.app.user_list.users.contains(user_id)) {
            fail(fmt::format("Player {}'s Vita user ({}) could not be created. See {}.", player, user_id,
                fs_utils::path_to_utf8(data_dir / "vita3k.log")));
            return 1;
        }
        emuenv.cfg.user_id = user_id;
        std::string message;
        if (ss_delta::copy_save_to_user(settings.vita_fs, "00", user_id, message))
            LOG_INFO("Save: {}", message);
        else
            LOG_ERROR("Save: {}", message);
    }

    if (!fs::exists(emuenv.vita_fs_path / "ux0" / "app" / TITLE_ID / "eboot.bin")) {
        fail(fmt::format("Soul Sacrifice Delta ({}) is not installed in {}.\nSet [paths] vita_fs in ss-delta.ini.",
            TITLE_ID, emuenv.vita_fs_path.string()));
        return 1;
    }

    // add-ons: the game reads what was bought on PSN from the save's system.bin; the content itself
    // is in the game's data (docs/decisions.md D14 in ss-delta)
    if (settings.dlc) {
        std::string why;
        if (ss_delta::addon_data_present(emuenv.vita_fs_path / "ux0" / "app" / TITLE_ID, why)) {
            std::string message;
            const bool ok = ss_delta::unlock_addons(settings.vita_fs, data_dir / "save-backups", message);
            if (ok)
                LOG_INFO("Add-ons: {}", message);
            else
                LOG_ERROR("Add-ons: {}", message);
        } else
            LOG_WARN("Add-ons: not unlocked, the installed game lacks them ({})", why);
    }

    // the game's own engine settings that ss-delta.ini changes
    std::map<std::string, std::string> boot_config{ { "ResoDown", settings.full_3d ? "0" : "1" } };
    for (const auto &[key, value] : settings.boot_config)
        boot_config[key] = value;
    override_boot_config(emuenv, data_dir / "game", boot_config);
    if (settings.full_3d)
        emuenv.gxm.on_program_registered = unscale_depth_uvs;

    // window: size and fullscreen from the settings, F11 or Alt+Enter switch, frame rate in the title
    SDL_Window *window = nullptr;
    ss_delta::Keyboard keyboard;
    ss_delta::Touches touches;
    {
        std::vector<std::string> touch_warnings;
        touches.configure(settings.touches, touch_warnings);
        for (const auto &line : touch_warnings)
            LOG_WARN("ss-delta.ini: {}", line);
    }
    auto last_title = std::chrono::steady_clock::now();
    sdl_frontend::Hooks hooks;
    hooks.on_window = [&](SDL_Window *w) {
        window = w;
        if (!window)
            return;
        SDL_SetWindowTitle(window, window_name.c_str());
        // the game's own icon, from the player's installed copy
        const fs::path icon_path = emuenv.vita_fs_path / "ux0" / "app" / TITLE_ID / "sce_sys" / "icon0.png";
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
                save_frame(emuenv, data_dir / "screenshots" / fmt::format("ss-delta_{:%Y-%m-%d_%H%M%S}.png", local));
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
    int frame_rate = 30; // what update_frame_rate last set (-1: left alone)
    hooks.on_tick = [&] {
        if (dump_elfs_seconds > 0 && std::chrono::steady_clock::now() - session_start > std::chrono::duration<double>(dump_elfs_seconds)) {
            // --dump-elfs: the modules the game loads at boot are written. Ad Hoc Play's matching
            // module loads only when that menu opens: load it now so it is written too (it is
            // recompiled for the address it takes in play, tools/install-game.ps1). Then quit.
            if (!load_sys_module(emuenv, SCE_SYSMODULE_NET_ADHOC_MATCHING))
                LOG_ERROR("--dump-elfs: the ad hoc matching module could not be loaded");
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
            dump_elfs_seconds = 0;
        }
        if (settings.frame_rate != 30 && frame_rate != -1)
            frame_rate = update_frame_rate(emuenv, settings.frame_rate, frame_rate);
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
        const auto title = fmt::format("{} | {} | {} FPS ({} ms) | {}x{} | {}", window_name, cc.backend_renderer,
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
