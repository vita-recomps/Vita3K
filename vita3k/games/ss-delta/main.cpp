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

// ss-delta.exe: Soul Sacrifice Delta (PCSA00152) on the shared single-game frontend
// (game-frontend/profile.h). This file holds what is particular to the game: its settings, save
// import and export, the add-on unlock, Ad Hoc Play, its engine settings and its frame rate.

#include "../../game-frontend/profile.h"
#include "dlc.h"
#include "saves.h"

#include <config/state.h>
#include <ctrl/state.h>
#include <emuenv/state.h>
#include <gxm/state.h>
#include <gxm/types.h>
#include <io/functions.h>
#include <io/state.h>
#include <mem/functions.h>
#include <mem/ptr.h>
#include <modules/module_parent.h>
#include <util/log.h>
#include <util/net_utils.h>

#include <SDL3/SDL.h>
// provides WinMain for the windowed executable, which calls main below
#include <SDL3/SDL_main.h>

#include <cstring>
#include <iterator>
#include <map>
#include <set>
#include <string>

namespace {

using game_frontend::IniSection;
using game_frontend::SettingSpec;
using Kind = SettingSpec::Kind;
namespace layout = game_frontend::layout;

constexpr const char *TITLE_ID = "PCSA00152";

game_frontend::IniLayout ini_layout() {
    const std::string id = "ss-delta", exe = "ss-delta.exe";
    IniSection display{ "display", "",
        {
            { "full_3d", Kind::Bool, "true", 0, 0, {},
                "Draw the 3D at 960x544 like the menus and HUD (the game's own ResoDown 0). false: 720x408,\n"
                "upscaled, as on the Vita." },
            layout::scale(),
            layout::window_scale(),
            layout::fullscreen(id, exe),
            layout::vsync(),
            { "frame_rate", Kind::Choice, "60", 0, 0, { "30", "60" },
                "The game's frame rate: 60, or 30 as on the Vita. The game keeps its speed at 60; loading\n"
                "screens still run at 30 (they don't finish at 60)." },
            layout::filter(),
        } };
    IniSection game{ "game", "",
        { { "dlc", Kind::Bool, "true", 0, 0, {},
            "Unlock the add-ons (raiments, hoods, additional and otherworldly pacts). Their content is in the\n"
            "game's own data since update 1.30; PSN, which recorded the purchases, is closed. The save's\n"
            "system.bin is changed once (a copy goes to ss-delta/save-backups/)." } } };
    IniSection multiplayer{ "multiplayer", "",
        {
            { "network", Kind::Choice, "local", 0, 0, { "local", "lan" },
                "Ad Hoc Play (Librom, Network). Start one copy per player: ss-delta.exe is player 1, and\n"
                "\"ss-delta.exe --player 2\" (run-player2.bat) player 2, up to 4. Each player has a Vita user and\n"
                "saves of its own (player 1's are copied on its first start), and its log and config in\n"
                "ss-delta/player<N>/. ss-delta-player<N>.ini next to this file overrides settings for player N.\n"
                "local: the copies run on this PC. lan: on PCs in one network, as in Vita3K (UDP broadcasts on\n"
                "the network adapter numbered `adapter`, 0 the first; allow ss-delta.exe through the firewall)." },
            { "adapter", Kind::Int, "0", 0, 63, {}, "", true },
            { "gamepad", Kind::Int, "0", 0, 4, {},
                "0: every gamepad drives the window in focus. 1-4: only that gamepad, also while the window is in\n"
                "the background (two players at one PC, a gamepad each; set it in ss-delta-player<N>.ini)." },
        } };
    IniSection boot_config{ "boot_config",
        "Engine settings passed to the game's boot_config.ini (Key = value, case as in that file), for\n"
        "experiments; they win over the settings above. The installed file is not changed." };
    boot_config.free_keys = boot_config.keep_case = true;
    boot_config.body = ";FPS = 30\n";
    return {
        "ss-delta settings. Lines starting with ; are comments; delete the ; to use a line.",
        {
            layout::paths(TITLE_ID, id, exe),
            display,
            layout::audio(),
            game,
            multiplayer,
            layout::recompiled(id),
            layout::debug(id),
            layout::keyboard(),
            layout::gamepad(),
            layout::touch("; Black Rite: touch and hold the Black Rite icon, lower left during a quest.\n"
                          "black_rite = 56, 494\nblack_rite_key = F\nblack_rite_gamepad = lefttrigger\n"),
            boot_config,
        },
    };
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

// --import-save / --export-save
struct SaveCommand {
    std::string import_from, export_to;
    int slot = 1;
    bool quiet = false;
};

} // namespace

int main(int argc, char *argv[]) {
    // the game's own command line, besides the frontend's (game-frontend/frontend.cpp):
    //   --import-save <folder|zip> copy a save (decrypted, e.g. from a Vita) in, keeping the old one
    //   --export-save <folder>     copy a save out
    //   --slot <1-3>               the game's save slot for the two above (default 1)
    //   --quiet                    no message box for the two above (the result is in the log)
    SaveCommand save;
    int frame_rate = 30; // what update_frame_rate last set (-1: left alone)

    game_frontend::Profile profile;
    profile.title_id = TITLE_ID;
    profile.name = "Soul Sacrifice Delta";
    profile.id = "ss-delta";
    profile.ini = ini_layout();
    profile.default_touches = { { "black_rite", 56, 494, "F", "lefttrigger" } };
    profile.multiplayer = true;
    profile.icon_resource = true;

    auto &hooks = profile.hooks;
    hooks.parse_arg = [&](game_frontend::Context &, const std::vector<std::string> &args, size_t &i) {
        const std::string &arg = args[i];
        const bool has_value = i + 1 < args.size();
        if (arg == "--import-save" && has_value)
            save.import_from = args[++i];
        else if (arg == "--export-save" && has_value)
            save.export_to = args[++i];
        else if (arg == "--slot" && has_value)
            save.slot = std::atoi(args[++i].c_str());
        else if (arg == "--quiet")
            save.quiet = true;
        else
            return false;
        return true;
    };
    hooks.before_init = [&](game_frontend::Context &ctx) -> std::optional<int> {
        ss_delta::set_save_user(ctx.user_id);
        if (save.import_from.empty() && save.export_to.empty())
            return std::nullopt;
        const auto &vita_fs = ctx.settings->vita_fs;
        std::string message;
        const bool ok = !save.import_from.empty()
            ? ss_delta::import_save(vita_fs, fs_utils::utf8_to_path(save.import_from), save.slot, ctx.data_dir / "save-backups", message)
            : ss_delta::export_save(vita_fs, fs_utils::utf8_to_path(save.export_to), save.slot, message);
        if (ok)
            LOG_INFO("Save: {}", message);
        else
            LOG_ERROR("Save: {}", message);
        if (!save.quiet)
            SDL_ShowSimpleMessageBox(ok ? SDL_MESSAGEBOX_INFORMATION : SDL_MESSAGEBOX_ERROR, ctx.window_name.c_str(),
                ("Save: " + message).c_str(), nullptr);
        return ok ? 0 : 1;
    };
    hooks.configure = [&](game_frontend::Context &ctx, Config &cfg) {
        const auto &values = ctx.settings->values;
        // Ad Hoc Play: copies on this PC talk over 127.0.0.<player>; on a network, through an
        // adapter (Vita3K's adhoc-addr). Players 2 and up only read the shader cache player 1 keeps.
        net_utils::set_local_adhoc(values.get("multiplayer.network") == "lan" ? 0 : ctx.player);
        cfg.adhoc_addr = values.get_int("multiplayer.adapter");
        cfg.shader_cache_read_only = ctx.player > 1;
        // [multiplayer] gamepad: one gamepad per copy of the game, read also while its window is in
        // the background
        if (values.get_int("multiplayer.gamepad") > 0)
            SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    };
    hooks.before_run = [&](game_frontend::Context &ctx) {
        EmuEnvState &emuenv = *ctx.emuenv;
        const auto &settings = *ctx.settings;
        const auto &values = settings.values;
        emuenv.ctrl.only_gamepad = values.get_int("multiplayer.gamepad");
        if (ctx.player > 1) {
            std::string message;
            if (ss_delta::copy_save_to_user(settings.vita_fs, "00", ctx.user_id, message))
                LOG_INFO("Save: {}", message);
            else
                LOG_ERROR("Save: {}", message);
        }
        // add-ons: the game reads what was bought on PSN from the save's system.bin; the content
        // itself is in the game's data (docs/decisions.md D14 in ss-delta)
        if (values.get_bool("game.dlc")) {
            std::string why;
            if (ss_delta::addon_data_present(emuenv.vita_fs_path / "ux0" / "app" / TITLE_ID, why)) {
                std::string message;
                if (ss_delta::unlock_addons(settings.vita_fs, ctx.data_dir / "save-backups", message))
                    LOG_INFO("Add-ons: {}", message);
                else
                    LOG_ERROR("Add-ons: {}", message);
            } else
                LOG_WARN("Add-ons: not unlocked, the installed game lacks them ({})", why);
        }
        // the game's own engine settings that ss-delta.ini changes
        const bool full_3d = values.get_bool("display.full_3d");
        std::map<std::string, std::string> boot_config{ { "ResoDown", full_3d ? "0" : "1" } };
        for (const auto &[key, value] : values.section("boot_config"))
            boot_config[key] = value;
        override_boot_config(emuenv, ctx.data_dir / "game", boot_config);
        if (full_3d)
            emuenv.gxm.on_program_registered = unscale_depth_uvs;
    };
    hooks.on_tick = [&](game_frontend::Context &ctx) {
        const int fps = ctx.settings->values.get_int("display.frame_rate");
        if (fps != 30 && frame_rate != -1)
            frame_rate = update_frame_rate(*ctx.emuenv, fps, frame_rate);
    };
    hooks.before_dump_quit = [&](game_frontend::Context &ctx) {
        // Ad Hoc Play's matching module loads only when that menu opens: load it now so it is
        // written too (it is recompiled for the address it takes in play, tools/install-game.ps1)
        if (!load_sys_module(*ctx.emuenv, SCE_SYSMODULE_NET_ADHOC_MATCHING))
            LOG_ERROR("--dump-elfs: the ad hoc matching module could not be loaded");
    };

    return game_frontend::run(profile, argc, argv);
}
