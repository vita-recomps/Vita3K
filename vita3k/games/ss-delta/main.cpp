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

// ss-delta.exe: Soul Sacrifice Delta (PCSA00152 US, PCSF00532 EU) on the shared single-game frontend
// (game-frontend/profile.h). This file holds what is particular to the game: its settings, save
// import and export, the add-on unlock, Ad Hoc Play, its engine settings and its frame rate.

#include "../../game-frontend/button_labels.h"
#include "../../game-frontend/profile.h"
#include "allies.h"
#include "dlc.h"
#include "saves.h"
#include "frame_pacing.h"

#include <config/state.h>
#include <cpu/aot.h>
#include <ctrl/state.h>
#include <emuenv/state.h>
#include <gxm/state.h>
#include <gxm/types.h>
#include <io/functions.h>
#include <io/state.h>
#include <mem/functions.h>
#include <mem/ptr.h>
#include <modules/module_parent.h>
#include <renderer/state.h>
#include <renderer/texture_cache.h>
#include <util/fs.h>
#include <util/log.h>
#include <util/net_utils.h>
#include <util/string_utils.h>

#include <SDL3/SDL.h>
#include <stb_image_write.h>
// provides WinMain for the windowed executable, which calls main below
#include <SDL3/SDL_main.h>

#include <cmath>
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

// The regions the port runs, at update 1.30 (the frontend runs the one installed). Their eboots are
// one build, the EU one linked 31 s before the US one: the code is byte for byte the same, and the
// data differs only in build stamps (ss-delta docs/01-research.md, "EU version"). So every guest
// address below holds for both, and the recompiled modules are the same.
struct Region {
    const char *title_id;
    const char *name;
};
constexpr Region REGIONS[] = {
    { "PCSA00152", "US" },
    { "PCSF00532", "EU" },
};
constexpr const char *TITLE_ID = REGIONS[0].title_id;

const char *region_name(const std::string &title_id) {
    for (const auto &r : REGIONS)
        if (title_id == r.title_id)
            return r.name;
    return "unknown";
}

// [multiplayer] adapter: Vita3K's adhoc_addr, an index into this PC's network adapters with an
// address (net_utils::get_all_assigned_addrs, the list Vita3K's own settings show). The settings
// menu lists them as "address - name" (Vita3K's order; a long name is cut, not the address);
// auto (the default, adhoc_addr -1): the adapter the machine reaches the first of the peers through,
// or without peers the one of its default route (net_utils::get_selected_assigned_addr).
SettingSpec adapter_setting() {
#ifdef __ANDROID__
    const char *const help = "The network adapter Ad Hoc Play uses: the one in the same network as the other players\n"
                             "(usually the Wi-Fi). auto: the one that reaches the first of the peers, else the internet's.";
#else
    const char *const help = "The network adapter LAN play uses (network = lan): the one in the same network as the other\n"
                             "PCs. auto: the one that reaches the first of the peers (Tailscale's for a Tailscale address),\n"
                             "else the one of the internet connection (the home network's).";
#endif
    SettingSpec s{ "adapter", Kind::Choice, "auto", 0, 0, { "auto" }, help, true };
    s.title = "Network adapter";
    s.choice_labels.push_back("auto");
    const auto addrs = net_utils::get_all_assigned_addrs();
    for (size_t i = 0; i < addrs.size(); i++) {
        s.choices.push_back(std::to_string(i));
        s.choice_labels.push_back(addrs[i].name.empty() ? addrs[i].addr : addrs[i].addr + " - " + addrs[i].name);
    }
    return s;
}

// [multiplayer] adapter as Vita3K's adhoc_addr: auto is -1
int adapter_index(const std::string &value) {
    if (value.empty() || string_utils::tolower(value) == "auto")
        return -1;
    return std::max(0, std::atoi(value.c_str()));
}

// [multiplayer] peers: typed in the settings menu too (a keyboard, or Up/Down through digits, dots,
// letters for a host name and commas), and applied at once (frontend.cpp)
SettingSpec peers_setting() {
    SettingSpec s{ "peers", Kind::Text, "", 0, 0, {},
        "lan, over a VPN (Tailscale, ZeroTier, Radmin VPN) or another network that passes no broadcasts: the\n"
        "host's VPN address (or the other players', separated by commas). Each broadcast also goes to each,\n"
        "and to every address a packet came from, so the host needs none (docs/ONLINE-PLAY.md)." };
    s.title = "Peers (host address)";
    s.live = true;
    s.text_entry = true;
    s.text_chars = "0123456789.,abcdefghijklmnopqrstuvwxyz-";
    return s;
}

// [game] language: the Vita's system language, which the game reads once at boot. It has no
// language option of its own: the US region shows French, Spanish or Portuguese (Brazil) for those
// system languages and English for any other; the EU region French, Spanish, German or Italian,
// and English (UK) otherwise (ss-delta docs/01-research.md, "Text language").
const std::map<std::string, SceSystemParamLang> LANGUAGES{
    { "english", SCE_SYSTEM_PARAM_LANG_ENGLISH_US },
    { "french", SCE_SYSTEM_PARAM_LANG_FRENCH },
    { "spanish", SCE_SYSTEM_PARAM_LANG_SPANISH },
    { "german", SCE_SYSTEM_PARAM_LANG_GERMAN },
    { "italian", SCE_SYSTEM_PARAM_LANG_ITALIAN },
    { "portuguese", SCE_SYSTEM_PARAM_LANG_PORTUGUESE_BR },
};

SettingSpec language_setting() {
    SettingSpec s{ "language", Kind::Choice, "english", 0, 0, { "english", "french", "spanish", "german", "italian", "portuguese" },
        "The game's language, also in the system dialogs: english, french, spanish, german, italian,\n"
        "portuguese. The US game has English, French, Spanish and Portuguese (Brazil), the EU game\n"
        "English, French, Spanish, German and Italian; one the installed game lacks gives English." };
    s.title = "Language";
    s.choice_labels = { "English", "French", "Spanish", "German (EU game)", "Italian (EU game)", "Portuguese (US game)" };
    return s;
}

game_frontend::IniLayout ini_layout() {
    const std::string id = "ss-delta", exe = id + game_frontend::executable_suffix;
    IniSection display{ "display", "",
        {
            { "full_3d", Kind::Bool, "true", 0, 0, {},
                "Draw the 3D at 960x544 like the menus and HUD (the game's own ResoDown 0). Off: 720x408,\n"
                "upscaled, as on the Vita." },
            layout::scale(),
            layout::window_scale(),
            layout::fullscreen(id, exe),
            layout::vsync(),
            { "frame_rate", Kind::Choice, "60", 0, 0, { "24", "30", "40", "45", "60" },
                "Target frame rate: 24, 30, 40, 45 or 60. Game speed uses measured frame time.\n"
                "60 is experimental; lower rates were tested by the owner. Cadence depends on device/vblank.\n"
                "Loading screens fall back to 30 only with a build lacking the loading-race fix." },
            layout::filter(),
            layout::fps_counter(),
            // off: no visible change on the shimmering ST17 ground (the owner, 2026-10-14), so its cause
            // there is something else (**hypothesis**: explicit-LOD sampling or geometry)
            layout::mipmaps(false),
        } };
    // the settings menu changes the frame rate while the game runs: on_tick reads it every tick
    for (auto &spec : display.settings) {
        if (spec.key == "frame_rate") { spec.live = true; spec.choice_labels = { "24", "30", "40", "45", "60 (experimental)" }; }
        if (spec.key == "full_3d")
            spec.title = "Full-resolution 3D";
    }
    IniSection game{ "game", "",
        { { "dlc", Kind::Bool, "true", 0, 0, {},
            "Unlock the add-ons (raiments, hoods, additional and otherworldly pacts). Their content is in the\n"
            "game's own data since update 1.30; PSN, which recorded the purchases, is closed. The save's\n"
            "system.bin is changed once (a copy goes to ss-delta/save-backups/). The add-ons PSN gave as\n"
            "gifts at Cassus (Soulbinder, Necky and Slave raiments, the hoods ...) are given in the\n"
            "loaded save, as taking the gifts would, and Blank Page's five slots that needed Memory Clods\n"
            "(sold on the PlayStation Store) are unlocked; the game keeps them when it saves. Making a pact\n"
            "page with Sacrifice memory costs no Memory Clod." } } };
    game.settings[0].title = "Add-ons (DLC)";
    // GitHub issue #11, part 2: in the game's data, given only in Japan (Sylvan) or not at all (Benkei)
    game.settings.push_back({ "japan_raiments", Kind::Bool, "true", 0, 0, {},
        "With dlc, also give the raiments only Japan had: Sylvan (a gift there) and Benkei. Both are\n"
        "in the game's data; they stay in the save once it is written." });
    game.settings.back().title = "Japan-only raiments";
    // D29 in ss-delta: the original game's pre-order offerings, kept commented out in Delta's tables
    game.settings.push_back({ "spirits_offerings", Kind::Bool, "true", 0, 0, {},
        "With dlc, also give the original Soul Sacrifice's 20 pre-order offerings (Spirits' Flamepike ...\n"
        "Spirits' Gravelshield), once a save. Delta still has them, switched off: ss-delta switches them\n"
        "on in a copy of the game's patch archive in ux0/rePatch/ (Mods (rePatch) must be on). Applies at\n"
        "the next start; off takes them out of the game again." });
    game.settings.back().title = "Original's Spirits' offerings";
    // ss-delta 01-research, "Sortiara as an ally": a built-in mod, so off by default
    game.settings.push_back({ "sortiara_ally", Kind::Bool, "false", 0, 0, {},
        "Sortiara, who fights beside you in the early story, joins your allies (Portraiture > Allies),\n"
        "as a story ally joins after a quest. The game has all she needs as an ally (her offerings,\n"
        "voice, portrait) but never adds her. Given once a save; the game keeps her when it saves, and\n"
        "switching this off later doesn't take her away." });
    game.settings.back().title = "Sortiara as an ally";
    game.settings.back().live = true;
    game.settings.push_back({ "skip_intro", Kind::Bool, "false", 0, 0, {},
        "Skip the health warning and the company logos at start: the game opens on its title screen." });
    game.settings.back().title = "Skip intro logos";
    // GitHub issue #12 (ss-delta 01-research, "Touch-only spots"): the game asks sceKernelGetModel
    // whether it runs on a PS TV and then takes buttons where a Vita needs a touch
    game.settings.push_back({ "pstv_mode", Kind::Bool, "false", 0, 0, {},
        "Run as a PS TV, which has no touchscreen: the game then takes L2 or R2 (a gamepad's triggers,\n"
        "U or O on the keyboard, [keyboard] l2 and r2) where a Vita needs a touch, e.g. pulling Librom\n"
        "out of the bones at the start of a new game. Read when the game starts." });
    game.settings.back().title = "PS TV controls";
    game.settings.push_back(language_setting());
    game.settings.push_back(game_frontend::button_labels_setting());
    // the Undub patch (Japanese voices, ss-delta docs/01-research.md, "Undub patch") is a rePatch mod
    game.settings.push_back(game_frontend::layout::repatch());
    // [cheats] (GitHub issue #7): switched in the settings menu while the game runs (apply_cheats)
    IniSection cheats{ "cheats",
        "Cheats, all off by default. They change the game's values while it runs; on the Cheats page of\n"
        "the settings menu they switch at once. What they change is kept when the game saves (Lacrima,\n"
        "levels), so export a save first if you want to go back (--export-save). In Ad Hoc Play they\n"
        "change only this player.",
        {
            { "infinite_health", Kind::Bool, "false", 0, 0, {},
                "In quests, your health is refilled to its maximum as soon as it drops. A hit that takes it\n"
                "all at once can still bring you down." },
            { "infinite_offerings", Kind::Bool, "false", 0, 0, {},
                "In quests, offerings don't run out: each cast is given back at once, and one already lost\n"
                "comes back. How long a cast lasts stays as it is (see Lasting offerings)." },
            { "long_offerings", Kind::Bool, "false", 0, 0, {},
                "Offerings whose effect lasts a while (giant arms and weapons, armour, buffs) keep it until\n"
                "the quest ends instead of fading after their usual time. Counts from the next cast." },
            { "one_hit_kills", Kind::Bool, "false", 0, 0, {},
                "In quests, your attacks defeat ordinary enemies in one hit. Bosses take five: the game\n"
                "limits a hit on a boss to a fifth of its health." },
            { "invincible", Kind::Bool, "false", 0, 0, {},
                "Enemies' attacks neither hurt you nor knock you down: hits pass without a stagger, so you\n"
                "keep control. Attacks that cost health cost none either. The game's own debug switch\n"
                "(NoDamage); switched off, hits count again at once." },
            { "max_lacrima", Kind::Bool, "false", 0, 0, {}, "Keep Lacrima at 9,999,999." },
            { "max_levels", Kind::Bool, "false", 0, 0, {},
                "Raise the Life and Magic levels to 99, the highest. Not yet early in the story (the\n"
                "journal's first chapters, while Portraiture hides the Right Arm's levels)." },
        } };
    for (auto &spec : cheats.settings)
        spec.live = true;
    cheats.settings[1].title = "Unlimited offerings";
    cheats.settings[2].title = "Lasting offerings";
    cheats.settings[3].title = "One-hit kills";
    cheats.settings[4].title = "Invincible";
    cheats.settings[5].title = "Max Lacrima";
    cheats.settings[6].title = "Max levels";
    // Windows starts with local, so a first start doesn't raise a firewall prompt; a Steam Deck or a Mac is
    // rarely used for several copies on one machine, and with local its rooms never reached the network
#ifdef _WIN32
    const char *const network_default = "local";
#else
    const char *const network_default = "lan";
#endif
    IniSection multiplayer{ "multiplayer", "",
        {
            { "network", Kind::Choice, network_default, 0, 0, { "local", "lan" },
                "Ad Hoc Play (Librom, Network). local (the default on Windows): copies of the game on this\n"
                "machine play together. lan (the default on Linux and macOS): machines in one network play\n"
                "together (Windows, Linux and macOS mixed), as in Vita3K: the copies find each other by UDP\n"
                "broadcasts on the network adapter set by `adapter`; over Tailscale, through `peers`. Windows:\n"
                "allow ss-delta.exe through the firewall. macOS: allow Local Network access when it asks.\n"
                "Start one copy per player: ss-delta.exe is player 1, and \"ss-delta.exe --player 2\"\n"
                "(run-player2.bat) player 2, up to 4. Each player has a Vita user and saves of its own (player\n"
                "1's are copied on its first start), and its log and config in ss-delta/player<N>/.\n"
                "ss-delta-player<N>.ini next to this file overrides settings for player N." },
            adapter_setting(),
            peers_setting(),
            { "gamepad", Kind::Int, "0", 0, 4, {},
                "0: every gamepad drives the window in focus. 1-4: only that gamepad, also while the window is in\n"
                "the background (two players at one PC, a gamepad each; set it in ss-delta-player<N>.ini)." },
        } };
    // Android runs one copy of the game, so its Ad Hoc Play is always lan (network's default there)
    for (auto &spec : multiplayer.settings)
        if (spec.key == "network" || spec.key == "gamepad")
            spec.desktop_only = true;
    IniSection boot_config{ "boot_config",
        "Engine settings passed to the game's boot_config.ini (Key = value, case as in that file), for\n"
        "experiments; they win over the settings above. The installed file is not changed." };
    boot_config.free_keys = boot_config.keep_case = true;
    boot_config.body = ";FPS = 30\n";
    return {
        "ss-delta settings. Lines starting with ; are comments; delete the ; to use a line.",
        {
            layout::paths("PCSA00152 for the US game, PCSF00532 for the EU one", id, exe),
            display,
            layout::audio(),
            game,
            cheats,
            multiplayer,
            layout::recompiled(id),
            layout::debug(id),
            layout::keyboard(),
            layout::gamepad(),
#ifndef __ANDROID__
            // the mouse camera: the right stick turns the camera in the field (a PC's mouse)
            layout::mouse(),
#endif
            layout::touch("; Black Rite: touch and hold the Black Rite icon, lower left during a quest.\n"
                          "black_rite = 56, 494\nblack_rite_key = F\nblack_rite_gamepad = lefttrigger\n"),
            boot_config,
        },
    };
}

// The game reads its engine settings from app0:resource/boot_config.ini, "Key<tab>Value" lines in
// two blocks (development, then retail). Serves a copy, written to `dir`, in which each key of
// `values` has the given value everywhere it is set; the installed file stays as it is.
void override_boot_config(EmuEnvState &emuenv, const std::string &title_id, const fs::path &dir, const std::map<std::string, std::string> &values) {
    const fs::path installed = emuenv.vita_fs_path / "ux0" / "app" / title_id / "resource" / "boot_config.ini";
    // a rePatch mod's copy, when it has one (game_frontend::apply_repatch ran before)
    const auto modded = emuenv.io.read_overrides.find(read_override_key(installed));
    fs::ifstream in(modded == emuenv.io.read_overrides.end() ? installed : modded->second, std::ios::binary);
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
// by the measured frame time, so it keeps its speed at 60 fps. The quest-start loading screen
// didn't finish with frames shorter than about 1/45 s: the loader works a step a frame and can be
// done while the screen's opening still plays, and the one call that ends the "roading_loop"
// animation (0x812b50ce) then comes before the loop starts, which then never ends. The game's code
// is fixed for that in the recompiled eboot (ss-delta overrides/eboot.json, feature "loading_race",
// D16/D30). An eboot without it (built before the fix, or run on the JIT) still gets the old way:
// loading screens at 30 fps, while the NowLoadingTask singleton (*0x81c9603c) is in state (+0xa4) 2,
// 3 or 5 (opening, "roading_loop", "roading_end"; 0x812b58ec). After a quest it plays the loop
// backwards (6) and rests in 4 until reset, so those stay at the set rate.
// The frame timer's period for a rate. At 60 it is 16000 us, not 16666: the game waits out the
// timer and then flips on the next vblank, and a host wait overshoots by up to about a millisecond,
// so with 16666 a few frames a second just missed their vblank and took two (57-58 fps on every GPU).
// The flip still waits for each vblank, so the game can't run faster than 60.
static uint64_t frame_period_us(int fps) {
    return fps == 60 ? 16000 : 1000000ull / fps;
}

// Called every frame; returns the rate it set (0: not yet).
int update_frame_rate(EmuEnvState &emuenv, int fps, int current) {
    if(!ss_delta::frame_pacing::supported(fps))return -1;
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
    const bool loading = state == 2 || state == 3 || state == 5;
    // asked when a loading screen starts: the eboot's native code is checked by then
    const bool loading_fixed = loading && aot_module_has_feature("eboot", "loading_race");
    static bool told = false;
    if (loading && !told) {
        told = true;
        LOG_INFO("frame_rate: loading screens {}", loading_fixed ? "at the set rate (the recompiled eboot has the fix)" : "at 30 fps (an eboot without the loading-screen fix)");
    }
    const int want = loading && !loading_fixed ? 30 : fps;
    if (want == current && *period == frame_period_us(want) && *flip == ss_delta::frame_pacing::flip_interval(want))
        return current;
    if (!ss_delta::frame_pacing::known_period(*period) || (*flip != 2 && *flip != 1)) {
        LOG_WARN("frame_rate: unexpected frame timer {} us / flip threshold {}; left alone", *period, *flip);
        return -1;
    }
    *period = frame_period_us(want);
    *flip = ss_delta::frame_pacing::flip_interval(want);
    LOG_INFO("frame_rate: {} fps{} (loading state {}, frame timer {} us, flip every {} vblank)", want, want != fps ? " for a loading screen" : "", state, *period, *flip);
    return want;
}

// [game] skip_intro (ss-delta docs/01-research.md, "Boot logos"): the boot's health warning and
// logos are GameLogoTasks that GameLogoControlTask (the singleton *0x81c957d0, built by 0x812ad8ce)
// keeps in a list (+0x50, count +0x48) and shows one after another. Each logo plays a timeline of
// floats in seconds: start delay +0x50, fade in +0x54, hold +0x58, fade out +0x5c, black +0x60;
// it is done (state +0x44 = 3) at its end, and the control task moves on, then starts the title.
// The timelines are data: set to 0, each logo ends on its first frame, before its fade-in. The
// vtables are checked, so nothing else is written. The constructor sets the singleton before it
// fills the list, so the list is read once the control task's init has run (its state +0x44 is 1,
// then 2 when all are shown); the first logo then waits out its start delay (1/6 s) at most.
// Called every frame; returns the control task handled.
Address skip_intro_logos(EmuEnvState &emuenv, Address done) {
    constexpr Address CONTROL_HOLDER = 0x81c957d0;
    constexpr uint32_t CONTROL_VTABLE = 0x81c5ede0, LOGO_VTABLE = 0x81c5ee14;
    constexpr uint32_t CONTROL_RUNNING = 1, LOGO_DONE = 3;
    auto &mem = emuenv.mem;
    const auto u32 = [&](Address a) -> uint32_t * { return is_valid_addr(mem, a) ? Ptr<uint32_t>(a).get(mem) : nullptr; };
    const uint32_t *const holder = u32(CONTROL_HOLDER);
    if (!holder || !*holder || *holder == done)
        return done;
    const Address control = *holder;
    const uint32_t *const vtable = u32(control);
    const uint32_t *const state = u32(control + 0x44);
    const uint32_t *const count = u32(control + 0x48);
    const uint32_t *const list = u32(control + 0x50);
    if (!vtable || *vtable != CONTROL_VTABLE || !state || !count || !list)
        return done;
    if (*state != CONTROL_RUNNING)
        return *state > CONTROL_RUNNING ? control : done;
    if (!*list || !*count || *count > 8)
        return control;
    int skipped = 0;
    for (uint32_t i = 0; i < *count; i++) {
        const uint32_t *const entry = u32(*list + 4 * i);
        if (!entry || !*entry || !is_valid_addr(mem, *entry + 0x63))
            continue;
        uint32_t *const logo = Ptr<uint32_t>(*entry).get(mem);
        if (logo[0] != LOGO_VTABLE || logo[0x44 / 4] == LOGO_DONE)
            continue;
        for (uint32_t field = 0x50; field <= 0x60; field += 4)
            logo[field / 4] = 0; // 0.0f
        skipped++;
    }
    LOG_INFO("skip_intro: {} of {} boot logos skipped", skipped, *count);
    return control;
}

// [cheats] (GitHub issue #7; ss-delta docs/01-research.md, "Cheats"): values the game keeps in
// guest memory, set every frame while a cheat is on. Native code stays on, as for skip_intro.
// Addresses from the VitaCheat codes for 1.30 (r0ah, tomberyx), checked in our build:
//   * the quest's player: *(*0x81c98f00 + 0x18), set during a quest only (0 otherwise); the player
//     object's vtable is 0x81c61f6c, the quest's 0x81c7a40c. HP is a float at +0x58, its maximum an
//     int at +0xf8. Offering slots 0-5: uses left at +0x200 + 4 * slot (-1: empty slot, 9999: no
//     limit), the uses each started the quest with at +0x218 + 4 * slot.
//   * the save data: *0x81c98618 (the game's own pointer, set from boot). Lacrima at +0x8c, the
//     Life and Magic levels (u16, 1-99, boot/lowchaos_growth_param.csv) at +0x17ced8 and +0x17ceda.
//   * how long an offering's effect lasts is data in the game's tables, as it holds them from boot,
//     read at each cast (so a changed duration counts from the next cast):
//     - boot/arms_param.csv for the arms and weapons an offering puts on the right arm: records of
//       0x128 bytes, the skill id, the category, the control type, the duration in seconds as a
//       float at +0xc, the model name at +0x1c (Giant's Arm: 70 s, ws170_00);
//     - boot/magic_gimmick_param_ply.csv for summons, armour, healing and most other effects that
//       stay: records of 0x328 bytes, the skill id, a type, the lifetime float at +0x8 (the fire
//       golem: 30 s). The table also holds short-lived effects no offering lasts with, so only the
//       skills of offerings that have a duration are changed;
//     - boot/protector_param.csv for armour and shields (charms, shield shards): records of 0x11c
//       bytes; the duration float comes 4 bytes before the skill id, the model name "pa..." right
//       after it (the stone armour: 50 s, pa240_00.chr). The records are found by the skill id, so
//       anchor and field offsets count from it;
//     - boot/item_param_ply.csv, the offerings themselves: records of 0x6c bytes, the offering id,
//       the duration at +0x40, the skill at +0x5c, the cast limit at +0x64. Its duration is what the
//       game shows; for the buffs without a gimmick (charms, shield shards) it is the one used.
//     No pointer leads to the tables (only a tree of the game's tables), so each is found by a known
//     pair of records.
struct DurationTable {
    const char *name;
    uint32_t stride; // record size
    int32_t field; // the duration's offset from the record's address (the protectors': before it)
    Address anchor = 0; // a known record found in memory, or 0
};

struct CheatState {
    std::set<std::string> logged; // each cheat logs once per object (player, save data) it changes
    DurationTable arms{ "arms", 0x128, 0xc };
    DurationTable gimmicks{ "summon and effect", 0x328, 0x8 };
    DurationTable protectors{ "armour", 0x11c, -4 };
    DurationTable items{ "offering", 0x6c, 0x40 };
    int table_wait = 0; // ticks until the next search (the game loads its tables after boot)
    int table_searches = 0;
    int apply_wait = 0; // ticks until the tables are gone over again (once a second)
    Address lasting_text = 0; // "9999" in guest memory, for the buffs' durations (kept for the session)
    std::map<Address, uint32_t> saved_durations; // durations as the game had them, while changed
    bool no_damage_set = false; // the debug menu's NoDamage switch is on through the cheat
};

constexpr float LASTING_SECONDS = 9999.0f; // longer than any quest (the game's own limit is 60 minutes)

// a word of guest memory, or 0 when it isn't mapped
uint32_t word_at(MemState &mem, Address a) {
    return is_valid_addr_range(mem, a, a + 4) ? *Ptr<uint32_t>(a).get(mem) : 0;
}

float float_at(MemState &mem, Address a) {
    const uint32_t w = word_at(mem, a);
    float f;
    std::memcpy(&f, &w, 4);
    return f;
}

bool model_at(MemState &mem, Address a, const char *model) {
    const size_t n = std::strlen(model) + 1;
    return is_valid_addr_range(mem, a, a + static_cast<Address>(n)) && std::memcmp(Ptr<char>(a).get(mem), model, n) == 0;
}

// Two neighbouring records of each table, as in the game's data: arms (skill 1140000, ws140_00;
// then 1140100, ws140_01), gimmicks (80100, type 31, lifetime 0; then 81100, type 14, 0.5 s) and
// offerings (102000: skill 1102005, 15 casts; then 102030: 1102035, 6 casts)
bool arms_anchor(MemState &mem, Address a) {
    return word_at(mem, a) == 1140000 && model_at(mem, a + 0x1c, "ws140_00") && word_at(mem, a + 0x128) == 1140100 && model_at(mem, a + 0x128 + 0x1c, "ws140_01");
}

bool gimmicks_anchor(MemState &mem, Address a) {
    return word_at(mem, a) == 80100 && word_at(mem, a + 4) == 31 && float_at(mem, a + 8) == 0.0f
        && word_at(mem, a + 0x328) == 81100 && word_at(mem, a + 0x328 + 4) == 14 && float_at(mem, a + 0x328 + 8) == 0.5f;
}

// protectors: the stone armour (1240000, pa240_00.chr), then its next rank (1240010). Not by the
// duration in front of the id: the cheat changes it
bool protectors_anchor(MemState &mem, Address a) {
    return word_at(mem, a) == 1240000 && model_at(mem, a + 4, "pa240_00.chr")
        && word_at(mem, a + 0x11c) == 1240010 && model_at(mem, a + 0x11c + 4, "pa240_01.chr");
}

bool items_anchor(MemState &mem, Address a) {
    return word_at(mem, a) == 102000 && word_at(mem, a + 0x5c) == 1102005 && word_at(mem, a + 0x64) == 15
        && word_at(mem, a + 0x6c) == 102030 && word_at(mem, a + 0x6c + 0x5c) == 1102035 && word_at(mem, a + 0x6c + 0x64) == 6;
}

bool anchor_valid(MemState &mem, const CheatState &state, const DurationTable &table) {
    if (!table.anchor)
        return false;
    if (&table == &state.arms)
        return arms_anchor(mem, table.anchor);
    if (&table == &state.gimmicks)
        return gimmicks_anchor(mem, table.anchor);
    if (&table == &state.protectors)
        return protectors_anchor(mem, table.anchor);
    return items_anchor(mem, table.anchor);
}

// a record of the table: an arm's has a model name "ws..." and a duration of 0-10000 s; a
// gimmick's a skill id, a small type and a lifetime of -1 to 10000 s; an offering's an id, a cast
// limit 1-9999 and a skill id 1xxxxxx
bool is_duration_record(MemState &mem, const CheatState &state, const DurationTable &table, Address a) {
    if (!is_valid_addr_range(mem, a, a + table.stride))
        return false;
    if (&table == &state.arms) {
        const float duration = float_at(mem, a + 0xc);
        return word_at(mem, a) < 3000000 && std::memcmp(Ptr<char>(a + 0x1c).get(mem), "ws", 2) == 0 && duration >= 0 && duration <= 10000;
    }
    if (&table == &state.gimmicks) {
        const float lifetime = float_at(mem, a + 8);
        return word_at(mem, a) < 3000000 && word_at(mem, a + 4) < 1000 && lifetime >= -1 && lifetime <= 10000;
    }
    if (&table == &state.protectors) {
        const float duration = float_at(mem, a - 4);
        return word_at(mem, a) < 3000000 && is_valid_addr_range(mem, a - 4, a + 6) && std::memcmp(Ptr<char>(a + 4).get(mem), "pa", 2) == 0
            && duration >= 0 && duration <= 10000;
    }
    const uint32_t id = word_at(mem, a), skill = word_at(mem, a + 0x5c), casts = word_at(mem, a + 0x64);
    return id >= 100000 && id < 2000000 && casts >= 1 && casts <= 9999 && skill >= 1000000 && skill < 3000000;
}

// The anchors in one pass over the main heap, 4-byte aligned
void find_duration_tables(MemState &mem, CheatState &state) {
    constexpr Address START = 0x81000000, END = 0x90000000, PAGE = 0x1000;
    for (Address page = START; page < END && !(state.arms.anchor && state.gimmicks.anchor && state.protectors.anchor && state.items.anchor); page += PAGE) {
        if (!is_valid_addr(mem, page))
            continue;
        const uint32_t *const words = Ptr<uint32_t>(page).get(mem);
        for (uint32_t i = 0; i < PAGE / 4; i++) {
            const Address a = page + i * 4;
            if (!state.arms.anchor && words[i] == 1140000 && arms_anchor(mem, a))
                state.arms.anchor = a;
            else if (!state.gimmicks.anchor && words[i] == 80100 && gimmicks_anchor(mem, a))
                state.gimmicks.anchor = a;
            else if (!state.protectors.anchor && words[i] == 1240000 && protectors_anchor(mem, a))
                state.protectors.anchor = a;
            else if (!state.items.anchor && words[i] == 102000 && items_anchor(mem, a))
                state.items.anchor = a;
        }
    }
}

// the first record of the table: walked back from the anchor while the records look like its own
Address first_record(MemState &mem, const CheatState &state, const DurationTable &table) {
    Address first = table.anchor;
    while (is_duration_record(mem, state, table, first - table.stride))
        first -= table.stride;
    return first;
}

// The self-buffs (gimmick type 16: healing over time, speed, the falcon feathers' dash ...) take their
// duration from the record's "general parameter 1", which the game keeps as text: from +0x14 each
// parameter is a string object of 8 bytes, a class word (0x81c5abbc) and a pointer to the text in
// the table's string pool (the falcon feather: "80" at +0x20). The text is parsed at each cast. The
// pool is packed, so the pointer is pointed at "9999" in a page of guest memory of our own; the
// game's pointer goes back with the other durations. Only where the text is the offering's duration.
void apply_buff_durations(MemState &mem, CheatState &state, const std::map<uint32_t, float> &lasting_skills) {
    constexpr uint32_t BUFF_TYPE = 16, STRING_CLASS = 0x81c5abbc;
    int changed = 0;
    for (Address a = first_record(mem, state, state.gimmicks); is_duration_record(mem, state, state.gimmicks, a); a += state.gimmicks.stride) {
        const auto skill = lasting_skills.find(word_at(mem, a));
        if (skill == lasting_skills.end() || word_at(mem, a + 4) != BUFF_TYPE || word_at(mem, a + 0x1c) != STRING_CLASS)
            continue;
        const Address field = a + 0x20, text = word_at(mem, field);
        if (!text || text == state.lasting_text || !is_valid_addr_range(mem, text, text + 16))
            continue;
        char value[17] = {};
        std::memcpy(value, Ptr<char>(text).get(mem), 16);
        char *end = nullptr;
        const float seconds = std::strtof(value, &end);
        if (end == value || *end != '\0' || std::fabs(seconds - skill->second) > 0.01f)
            continue; // not this offering's duration
        if (!state.lasting_text) {
            state.lasting_text = alloc(mem, 16, "ss-delta cheats");
            if (!state.lasting_text) {
                LOG_WARN("cheats: lasting offerings: no guest memory for the buffs' duration; buffs keep theirs");
                return;
            }
            std::memcpy(Ptr<char>(state.lasting_text).get(mem), "9999", 5);
        }
        state.saved_durations.emplace(field, text);
        *Ptr<uint32_t>(field).get(mem) = state.lasting_text;
        changed++;
    }
    if (changed)
        LOG_INFO("cheats: lasting offerings: {} buffs last {} s from their next cast", changed, LASTING_SECONDS);
}

// [cheats] long_offerings: every offering's effect with a duration (arms, armour, buffs, summons)
// lasts 9999 s, past the end of any quest. The game's durations go back when the cheat is off.
void apply_offering_durations(EmuEnvState &emuenv, bool on, CheatState &state) {
    auto &mem = emuenv.mem;
    if (!on) {
        for (const auto &[field, value] : state.saved_durations)
            if (is_valid_addr_range(mem, field, field + 4))
                std::memcpy(Ptr<uint8_t>(field).get(mem), &value, 4);
        if (!state.saved_durations.empty())
            LOG_INFO("cheats: offering durations back as the game has them ({})", state.saved_durations.size());
        state.saved_durations.clear();
        state.apply_wait = 0;
        return;
    }
    for (DurationTable *table : { &state.arms, &state.gimmicks, &state.protectors, &state.items })
        if (table->anchor && !anchor_valid(mem, state, *table))
            table->anchor = 0;
    if (!state.arms.anchor || !state.gimmicks.anchor || !state.protectors.anchor || !state.items.anchor) {
        // searched about every 2 s until found: the tables load a few seconds after boot
        if (state.table_wait-- > 0)
            return;
        state.table_wait = 120;
        find_duration_tables(mem, state);
        if (!state.arms.anchor || !state.gimmicks.anchor || !state.protectors.anchor || !state.items.anchor) {
            if (++state.table_searches == 30)
                LOG_WARN("cheats: lasting offerings: the {} table isn't found after a minute; waiting for it",
                    !state.arms.anchor ? "arms" : !state.gimmicks.anchor ? "summon and effect" : !state.protectors.anchor ? "armour" : "offering");
            return;
        }
        LOG_INFO("cheats: duration tables: arms at {:#x}, summons and effects at {:#x}, armour at {:#x}, offerings at {:#x}",
            state.arms.anchor, state.gimmicks.anchor, state.protectors.anchor, state.items.anchor);
    }
    if (state.apply_wait-- > 0)
        return;
    state.apply_wait = 60;
    // the skills of the offerings that have a duration, with the duration as the game has it
    std::map<uint32_t, float> lasting_skills;
    for (Address a = first_record(mem, state, state.items); is_duration_record(mem, state, state.items, a); a += state.items.stride) {
        float duration = float_at(mem, a + 0x40);
        if (const auto saved = state.saved_durations.find(a + 0x40); saved != state.saved_durations.end())
            std::memcpy(&duration, &saved->second, 4);
        if (duration > 0)
            lasting_skills.emplace(word_at(mem, a + 0x5c), duration);
    }
    apply_buff_durations(mem, state, lasting_skills);
    for (const DurationTable *table : { &state.arms, &state.gimmicks, &state.protectors, &state.items }) {
        int changed = 0, records = 0;
        for (Address a = first_record(mem, state, *table); is_duration_record(mem, state, *table, a); a += table->stride, records++) {
            if (table == &state.gimmicks && !lasting_skills.contains(word_at(mem, a)))
                continue; // not an offering's lasting effect
            const Address field = static_cast<Address>(static_cast<int64_t>(a) + table->field);
            const float duration = float_at(mem, field);
            if (!(duration > 0) || duration >= 999)
                continue; // no duration (0), no limit (-1, or 999 for a few), or already changed
            state.saved_durations.emplace(field, word_at(mem, field));
            std::memcpy(Ptr<uint8_t>(field).get(mem), &LASTING_SECONDS, 4);
            changed++;
        }
        if (changed)
            LOG_INFO("cheats: lasting offerings: {} of {} {} records last {} s from their next cast", changed, records, table->name, LASTING_SECONDS);
    }
}

constexpr uint32_t LACRIMA_MAX = 9999999, LEVEL_MAX = 99;

void apply_cheats(EmuEnvState &emuenv, const game_frontend::Values &values, CheatState &state) {
    constexpr Address QUEST_HOLDER = 0x81c98f00, SAVE_HOLDER = 0x81c98618;
    constexpr uint32_t QUEST_VTABLE = 0x81c7a40c, PLAYER_VTABLE = 0x81c61f6c;
    auto &mem = emuenv.mem;
    const auto u32 = [&](Address a) -> uint32_t * { return is_valid_addr_range(mem, a, a + 4) ? Ptr<uint32_t>(a).get(mem) : nullptr; };
    const auto once = [&](const std::string &what, Address object) {
        if (state.logged.insert(fmt::format("{} {:#x}", what, object)).second)
            LOG_INFO("cheats: {} ({:#x})", what, object);
    };

    const bool health = values.get_bool("cheats.infinite_health");
    const bool offerings = values.get_bool("cheats.infinite_offerings");
    const bool one_hit = values.get_bool("cheats.one_hit_kills");
    apply_offering_durations(emuenv, values.get_bool("cheats.long_offerings"), state);

    // Invincible: the game keeps its developers' debug menu, a block at *0x81c9a488 built at boot.
    // Its Player group (from +0x68) has NoDamage at +0x8d, which the player's hit handler
    // (0x81377452, through 0x817e78fa) checks: a hit then neither takes health nor plays the hit
    // reaction (stagger, knock-down), and attacks that cost health cost none. Retail code never sets
    // it, so the cheat sets it while on and clears it once when switched off.
    const bool invincible = values.get_bool("cheats.invincible");
    if (invincible || state.no_damage_set) {
        constexpr Address DEBUG_HOLDER = 0x81c9a488;
        const uint32_t *const holder = u32(DEBUG_HOLDER);
        const Address block = holder ? *holder : 0;
        // the block's own defaults, so a wrong pointer is not written through: the Player group's
        // TurnLimit[Normal] (360.0 at +0x70) and MotionSpeed (1.0 at +0x88); the switch is a bool
        const bool valid = block && is_valid_addr_range(mem, block, block + 0x100) && float_at(mem, block + 0x70) == 360.0f
            && float_at(mem, block + 0x88) == 1.0f && Ptr<uint8_t>(block + 0x8d).get(mem)[0] <= 1;
        if (!valid) {
            // 0 early in the boot, before the game builds the block
            if (invincible && block && state.logged.insert(fmt::format("no debug block {:#x}", block)).second)
                LOG_WARN("cheats: invincible: the game's debug block isn't where expected ({:#x}); left alone", block);
        } else {
            uint8_t *const no_damage = Ptr<uint8_t>(block + 0x8d).get(mem);
            if (invincible) {
                if (*no_damage != 1) {
                    *no_damage = 1;
                    once("invincible", block);
                }
                state.no_damage_set = true;
            } else {
                *no_damage = 0;
                state.no_damage_set = false;
                state.logged.erase(fmt::format("invincible {:#x}", block));
                LOG_INFO("cheats: invincible off ({:#x})", block);
            }
        }
    }

    if (health || offerings || one_hit) {
        const uint32_t *const quest_holder = u32(QUEST_HOLDER);
        const uint32_t *const quest = quest_holder && *quest_holder ? u32(*quest_holder) : nullptr;
        const uint32_t *const player_ref = quest && *quest == QUEST_VTABLE ? u32(*quest_holder + 0x18) : nullptr;
        const Address player = player_ref ? *player_ref : 0;
        const uint32_t *const vtable = player ? u32(player) : nullptr;
        if (vtable && *vtable == PLAYER_VTABLE && is_valid_addr_range(mem, player, player + 0x400)) {
            uint8_t *const p = Ptr<uint8_t>(player).get(mem);
            if (health) {
                float hp;
                int32_t max_hp;
                std::memcpy(&hp, p + 0x58, 4);
                std::memcpy(&max_hp, p + 0xf8, 4);
                // not once health is 0: the game has started the player's death (or a rescue) by then
                if (hp > 0 && max_hp > 0 && max_hp < 1000000 && hp < static_cast<float>(max_hp)) {
                    const float full = static_cast<float>(max_hp);
                    std::memcpy(p + 0x58, &full, 4);
                    once("infinite health", player);
                }
            }
            if (offerings) {
                for (int slot = 0; slot < 6; slot++) {
                    int32_t left, start;
                    std::memcpy(&left, p + 0x200 + 4 * slot, 4);
                    std::memcpy(&start, p + 0x218 + 4 * slot, 4);
                    // -1 with a start: lost (cast at 0 uses, "... was lost"); an empty slot starts at 0.
                    // Writing the uses back brings a lost offering back (its icon, and it casts again)
                    if (left >= -1 && start > 0 && start < 9999 && left < start) {
                        std::memcpy(p + 0x200 + 4 * slot, &start, 4);
                        once("unlimited offering uses", player);
                    }
                }
            }
            if (one_hit) {
                // tomberyx's "One Hit Kill": 1.07e20 at +0x370 (a damage factor, **hypothesis**), which the
                // game sets back from time to time. Ordinary enemies then fall to one hit; the game still
                // limits a hit on a boss to a fifth of its health
                constexpr uint32_t ONE_HIT = 0x60babe00;
                uint32_t current;
                std::memcpy(&current, p + 0x370, 4);
                if (current != ONE_HIT) {
                    std::memcpy(p + 0x370, &ONE_HIT, 4);
                    once("one-hit kills", player);
                }
            }
        }
    }

    const bool lacrima = values.get_bool("cheats.max_lacrima");
    const bool levels = values.get_bool("cheats.max_levels");
    if (lacrima || levels) {
        const uint32_t *const save_holder = u32(SAVE_HOLDER);
        const Address save = save_holder ? *save_holder : 0;
        if (save && is_valid_addr_range(mem, save, save + 0x17cedc)) {
            uint8_t *const s = Ptr<uint8_t>(save).get(mem);
            uint32_t count;
            uint16_t life, magic;
            std::memcpy(&count, s + 0x8c, 4);
            std::memcpy(&life, s + 0x17ced8, 2);
            std::memcpy(&magic, s + 0x17ceda, 2);
            // values the game itself could hold, so a wrong pointer is not written through (at boot,
            // before a save is loaded, the block holds anything). Levels are 0 until the Right Arm's
            // levels open, early in the story.
            const bool plausible = count <= LACRIMA_MAX && life <= LEVEL_MAX && magic <= LEVEL_MAX;
            if (!plausible) {
                if (state.logged.insert(fmt::format("implausible {:#x}", save)).second)
                    LOG_WARN("cheats: unexpected save data at {:#x} (Lacrima {}, levels {} / {}); left alone", save, count, life, magic);
            } else {
                if (lacrima && count != LACRIMA_MAX) {
                    std::memcpy(s + 0x8c, &LACRIMA_MAX, 4);
                    once("max Lacrima", save);
                }
                // Early in the story (the journal's own chapters) these are 0 and the levels in play are in
                // another record, at a place that depends on the save (+0x139fa8 in one); left alone then
                if (levels && (!life || !magic) && state.logged.insert(fmt::format("no levels {:#x}", save)).second)
                    LOG_INFO("cheats: max levels: this save's levels aren't at the known place yet (early in the story); left alone");
                if (levels && life && magic && (life != LEVEL_MAX || magic != LEVEL_MAX)) {
                    const uint16_t max = LEVEL_MAX;
                    std::memcpy(s + 0x17ced8, &max, 2);
                    std::memcpy(s + 0x17ceda, &max, 2);
                    once("max levels", save);
                }
            }
        }
    }
}

// the game's textures with PlayStation buttons (the com_001_* icon atlases, tutorial pictures, the
// font page with the text's symbols) and the symbols in them, all languages (ss-delta
// tools/button-atlases.py; docs/01-research.md, "Button prompts")
std::vector<game_frontend::ButtonAtlas> button_atlases() {
    using Symbol = game_frontend::ButtonSymbol;
    return {
#include "button_atlases.inc"
    };
}

// --button-labels-test <in> <out>: the labels drawn on the one-texture GXT files in <in> (extracted
// from the game's archives, tools/lspk.py) as PNGs in <out>, at [display] scale; [gamepad] and [keyboard]
// as set. The labels of [game] button_labels (--set game.button_labels=keyboard, nintendo, or on Android
// handheld), Xbox ones for playstation and auto
int button_labels_test(game_frontend::ButtonLabels &labels, const game_frontend::Settings &settings, const fs::path &in, const fs::path &out) {
    const std::string set = settings.values.get("game.button_labels");
    const std::string style = set == "playstation" || set == "auto" ? "xbox" : set;
    labels.configure(style, settings.gamepad, static_cast<int>(std::lround(settings.scale)), {}, settings.keyboard);
    fs::create_directories(out);
    int written = 0;
    for (const auto &entry : fs::directory_iterator(in)) {
        if (entry.path().extension() != ".gxt")
            continue;
        std::vector<uint8_t> data;
        if (!fs_utils::read_data(entry.path(), data) || data.size() < 64)
            continue;
        // GXT: magic, version, texture count, ...; the first texture's offset, size, palette, flags,
        // type, format, width | height << 16
        uint32_t header[15];
        std::memcpy(header, data.data(), sizeof(header));
        const uint32_t offset = header[8], size = header[9], type = header[12];
        const auto format = static_cast<SceGxmTextureBaseFormat>(header[13] & 0x9f000000u);
        const uint32_t width = header[14] & 0xffff, height = header[14] >> 16;
        if (header[2] != 1 || type != SCE_GXM_TEXTURE_SWIZZLED || static_cast<size_t>(offset) + size > data.size() || !labels.wants(format, width, height))
            continue;
        std::vector<uint8_t> rgba;
        uint32_t w = 0, h = 0;
        if (!labels.make(format, width, height, data.data() + offset, size, rgba, w, h))
            continue;
        const fs::path png = out / (fs_utils::path_to_utf8(entry.path().stem()) + ".png");
        if (stbi_write_png(fs_utils::path_to_utf8(png).c_str(), w, h, 4, rgba.data(), w * 4))
            written++;
    }
    LOG_INFO("--button-labels-test: {} pictures in {}", written, fs_utils::path_to_utf8(out));
    return written > 0 ? 0 : 1;
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
    std::string labels_test_in, labels_test_out; // --button-labels-test
    // [game] button_labels: the textures with PlayStation buttons redrawn with Xbox labels as they are
    // uploaded (the renderer exists from the first ticks, before the game loads them); the settings menu
    // switches it
    game_frontend::ButtonLabels button_labels(button_atlases());
    int frame_rate = 30; // what update_frame_rate last set (-1: left alone)
    Address intro_skipped = 0; // the logo control task skip_intro_logos handled
    CheatState cheats;
    ss_delta::GiftState gifts; // [game] dlc: the add-ons PSN gave as gifts
    ss_delta::AllyState allies; // [game] sortiara_ally

    game_frontend::Profile profile;
    profile.title_id = TITLE_ID;
    for (const auto &r : REGIONS)
        if (r.title_id != profile.title_id)
            profile.other_title_ids.push_back(r.title_id);
    profile.name = "Soul Sacrifice Delta";
    profile.id = "ss-delta";
    profile.ini = ini_layout();
    profile.default_touches = { { "black_rite", 56, 494, "F", "lefttrigger", "Black Rite (touch)" } };
    // the lock-on is L held: on a phone's on-screen controls, a toggle holds it (the owner, 2026-11-21)
    profile.screen_latch = { SCE_CTRL_L1, "LOCK" };
    // how fast the right stick turns the camera (degrees a second, by push along one axis), measured with
    // the compass during a quest (ss-delta docs/01-research.md, "Mouse camera"): nothing up to 0.4, then
    // growing about tenfold for each 0.25 more, up to some 550 from 0.95
    profile.stick_response.points = {
        { 0.400f, 0.0f }, { 0.449f, 0.6f }, { 0.504f, 2.0f }, { 0.551f, 4.75f }, { 0.598f, 9.3f },
        { 0.701f, 27.8f }, { 0.756f, 47.0f }, { 0.803f, 73.0f }, { 0.850f, 119.0f }, { 0.874f, 156.0f },
        { 0.898f, 210.0f }, { 0.929f, 355.0f }, { 0.953f, 510.0f }, { 1.000f, 550.0f },
    };
    profile.multiplayer = true;
    profile.icon_resource = true;
    profile.usage = "Saves (then quit):\n"
                    "  --import-save <folder|zip>  copy a decrypted Vita save (e.g. from a Vita) in, keeping the\n"
                    "                              old one in ss-delta/save-backups/\n"
                    "  --export-save <folder>      copy a save out\n"
                    "  --slot <1-3>                the game's save slot for the two above (default 1)\n"
                    "  --quiet                     no message box for the two above (the result is in the log)\n"
                    "Development:\n"
                    "  --button-labels-test <in> <out>  the Xbox button labels (others with --set\n"
                    "                              game.button_labels=keyboard or nintendo) drawn on the textures\n"
                    "                              (.gxt) in <in>, as PNGs in <out>; then quit\n";

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
        else if (arg == "--button-labels-test" && i + 2 < args.size()) {
            labels_test_in = args[++i];
            labels_test_out = args[++i];
        } else if (arg == "--quiet")
            save.quiet = true;
        else
            return false;
        return true;
    };
    hooks.before_init = [&](game_frontend::Context &ctx) -> std::optional<int> {
        ss_delta::set_save_user(ctx.user_id);
        // the frontend has picked the installed region's title ID by now
        ss_delta::set_save_title(profile.title_id);
        LOG_INFO("Region: {} ({})", region_name(profile.title_id), profile.title_id);
        if (!labels_test_in.empty())
            return button_labels_test(button_labels, *ctx.settings, fs_utils::utf8_to_path(labels_test_in), fs_utils::utf8_to_path(labels_test_out));
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
        cfg.adhoc_addr = adapter_index(values.get("multiplayer.adapter"));
        // [multiplayer] peers: a broadcast also goes to each (lan mode; the settings menu changes them live, frontend.cpp)
        if (values.get("multiplayer.network") == "lan")
            net_utils::set_lan_peers_text(values.get("multiplayer.peers"));
        cfg.shader_cache_read_only = ctx.player > 1;
        // the fog colour leaves the vertex shaders as COLOR0/COLOR1 and goes above 1 (blue haze in ST02A
        // without the clamp; grey, as on the Vita, with it)
        cfg.clamp_color_varyings = true;
        // [game] language: the system language the game asks SceAppUtil for at boot
        cfg.sys_lang = LANGUAGES.at(values.get("game.language"));
        // [game] pstv_mode: the PS TV model (SceSysmem) and its pads on the external ports (SceCtrl;
        // the game reads port 0, which still takes the keyboard and the first gamepad)
        cfg.pstv_mode = values.get_bool("game.pstv_mode");
        // [multiplayer] gamepad: one gamepad per copy of the game, read also while its window is in
        // the background
        if (values.get_int("multiplayer.gamepad") > 0)
            SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    };
    // [game] spirits_offerings (D29 in ss-delta): their packaged mod made, or taken out, before the
    // rePatch mods are read; by player 1 only (Ad Hoc Play's copies share the file system)
    hooks.before_mods = [&](game_frontend::Context &ctx, bool repatch) {
        const auto &values = ctx.settings->values;
        const bool on = values.get_bool("game.dlc") && values.get_bool("game.spirits_offerings");
        if (ctx.player > 1)
            return;
        if (on && !repatch) {
            LOG_WARN("Spirits' offerings: not shown, Mods (rePatch) is off ([game] repatch = false)");
            return;
        }
        ss_delta::sync_spirits_offerings(ctx.emuenv->vita_fs_path, profile.title_id, on, ctx.data_dir);
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
            if (ss_delta::addon_data_present(emuenv.vita_fs_path / "ux0" / "app" / profile.title_id, why)) {
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
        override_boot_config(emuenv, profile.title_id, ctx.data_dir / "game", boot_config);
        if (full_3d)
            emuenv.gxm.on_program_registered = unscale_depth_uvs;
    };
    hooks.on_tick = [&](game_frontend::Context &ctx) {
        const int fps = ctx.settings->values.get_int("display.frame_rate");
        if (ss_delta::frame_pacing::supported(fps) && frame_rate != -1)
            frame_rate = update_frame_rate(*ctx.emuenv, fps, frame_rate);
        if (ctx.settings->values.get_bool("game.skip_intro"))
            intro_skipped = skip_intro_logos(*ctx.emuenv, intro_skipped);
        apply_cheats(*ctx.emuenv, ctx.settings->values, cheats);
        if (ctx.settings->values.get_bool("game.dlc"))
            ss_delta::grant_addon_gifts(*ctx.emuenv, gifts, ctx.settings->values.get_bool("game.japan_raiments"));
        if (ctx.settings->values.get_bool("game.sortiara_ally"))
            ss_delta::add_sortiara_ally(*ctx.emuenv, allies);
        if (renderer::TextureCache *cache = ctx.emuenv->renderer ? ctx.emuenv->renderer->get_texture_cache() : nullptr) {
            // auto: the device used last, and the first connected gamepad's own labels (both may change
            // while the game runs)
            std::string style = ctx.settings->values.get("game.button_labels");
            std::map<std::string, std::string> face;
            if (style == "auto")
                style = game_frontend::auto_button_style(ctx.emuenv->ctrl, face, ctx.last_input);
            const bool changed = button_labels.configure(style, ctx.settings->gamepad, static_cast<int>(std::lround(ctx.settings->scale)), face,
                    ctx.settings->keyboard);
            if (cache->texture_override.load() != &button_labels) {
                cache->texture_override = &button_labels;
                cache->refresh_texture_override();
            } else if (changed)
                cache->refresh_texture_override();
        }
    };
    hooks.before_dump_quit = [&](game_frontend::Context &ctx) {
        // Ad Hoc Play's matching module loads only when that menu opens: load it now so it is
        // written too (it is recompiled for the address it takes in play, tools/install-game.ps1)
        if (!load_sys_module(*ctx.emuenv, SCE_SYSMODULE_NET_ADHOC_MATCHING))
            LOG_ERROR("--dump-elfs: the ad hoc matching module could not be loaded");
    };

    return game_frontend::run(profile, argc, argv);
}
