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

#include <ctrl/ctrl.h>
#include <ctrl/functions.h>
#include <ctrl/state.h>

#include <config/state.h>
#include <display/functions.h>
#include <display/state.h>
#include <kernel/state.h>
#include <util/log.h>

#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#endif

static int reserve_port(CtrlState &state) {
    for (int i = 0; i < SCE_CTRL_MAX_WIRELESS_NUM; i++) {
        if (state.free_ports[i]) {
            state.free_ports[i] = false;
            return i;
        }
    }

    // No free port found.
    return -1;
}

SceCtrlExternalInputMode get_type_of_controller(CtrlState &state, int port) {
    for (const auto &[guid, controller] : state.controllers) {
        if (controller.port == port) {
            const auto type = SDL_GetGamepadType(controller.controller.get());
            return (type == SDL_GAMEPAD_TYPE_PS4) || (type == SDL_GAMEPAD_TYPE_PS5) ? SCE_CTRL_TYPE_DS4 : SCE_CTRL_TYPE_DS3;
        }
    }
    return SCE_CTRL_TYPE_DS3;
}

void refresh_controllers(CtrlState &state, EmuEnvState &emuenv) {
    // Remove disconnected controllers
    bool found_gyro = false;
    bool found_accel = false;
    const std::lock_guard lock(state.mutex);
    for (ControllerList::iterator controller = state.controllers.begin(); controller != state.controllers.end();) {
        if (SDL_GamepadConnected(controller->second.controller.get())) {
            found_accel |= controller->second.has_accel;
            found_gyro |= controller->second.has_gyro;

            ++controller;
        } else {
            state.free_ports[controller->second.port] = true;
            controller = state.controllers.erase(controller);
            state.controllers_num--;
        }
    }

    // Add new controllers
    int num_gamepads = 0;
    const auto gamepads = SDL_GetGamepads(&num_gamepads);
    for (int gamepad_index = 0; gamepad_index < num_gamepads; ++gamepad_index) {
        const auto gamepad_id = gamepads[gamepad_index];
        if (state.controllers_num >= SCE_CTRL_MAX_WIRELESS_NUM) {
            break;
        }
        if (state.only_gamepad > 0 && gamepad_index != state.only_gamepad - 1)
            continue;

#ifdef __ANDROID__
        // for whatever reasons, fingerprint sensors are detected as controllers, filter them out
        const char *controller_name = SDL_GetGamepadNameForID(gamepad_id);
        if (controller_name != nullptr && (std::string_view(controller_name).starts_with("uinput-") || std::string_view(controller_name).starts_with("gf_")))
            continue;
#endif

        const SDL_GUID guid = SDL_GetJoystickGUIDForID(gamepad_id);
        if (!state.controllers.contains(guid)) {
            Controller new_controller;
            new_controller.port = reserve_port(state);
            if (new_controller.port == -1) { // Port not available
                break;
            }
            const GamepadPtr controller(SDL_OpenGamepad(gamepad_id), SDL_CloseGamepad);
            if (controller == nullptr) {
                continue;
            }
            auto controller_name = SDL_GetGamepadName(controller.get());
            if (controller_name == nullptr) {
                continue;
            }
            new_controller.controller = controller;
            SDL_SetGamepadPlayerIndex(controller.get(), new_controller.port);
            new_controller.name = controller_name;

            new_controller.has_gyro = SDL_GamepadHasSensor(controller.get(), SDL_SENSOR_GYRO);
            if (new_controller.has_gyro)
                SDL_SetGamepadSensorEnabled(controller.get(), SDL_SENSOR_GYRO, true);
            new_controller.has_accel = SDL_GamepadHasSensor(controller.get(), SDL_SENSOR_ACCEL);
            if (new_controller.has_accel)
                SDL_SetGamepadSensorEnabled(controller.get(), SDL_SENSOR_ACCEL, true);

            new_controller.has_led = SDL_GetBooleanProperty(SDL_GetGamepadProperties(controller.get()), SDL_PROP_GAMEPAD_CAP_RGB_LED_BOOLEAN, false);
            if (new_controller.has_led) {
                auto &color = emuenv.cfg.controller_led_color;
                if (!color.empty()) {
                    color.resize(3);
                    SDL_SetGamepadLED(controller.get(), color[0], color[1], color[2]);
                }
            }

            found_gyro |= new_controller.has_gyro;
            found_accel |= new_controller.has_accel;

            state.controllers.emplace(guid, new_controller);
            state.controllers_num++;
        }
    }
    SDL_free(gamepads);
    state.has_motion_support = found_gyro && found_accel;
}

#ifdef _WIN32
// Test-only input injection. When VITA3K_INPUT_PIPE is set, a thread serves the named pipe
// \\.\pipe\<value> and applies one command per line to CtrlState::injected_state:
//   hold <button> [<button>...]   hold exactly these buttons ("hold" or "hold none" releases all)
//   axes <lx> <ly> <rx> <ry>      stick positions in [-1, 1]
//   touch <x> <y> ... | none      the frontend holds front-touch fingers at these points (960x544)
//   screenshot <path>             the frontend saves the current frame there, at render resolution
//   record <count> <dir>          the frontend saves the next <count> rendered frames into <dir>
//   memdump <address> <size> <path>  the frontend writes guest memory there (address, size in hex)
// Buttons: cross circle square triangle up down left right l r l2 r2 l3 r3 start select.
// Nothing goes through the window, so automated runs never need keyboard focus.
static uint32_t parse_injected_button(const std::string &name) {
    static const std::map<std::string, uint32_t> buttons = {
        { "cross", SCE_CTRL_CROSS }, { "circle", SCE_CTRL_CIRCLE }, { "square", SCE_CTRL_SQUARE },
        { "triangle", SCE_CTRL_TRIANGLE }, { "up", SCE_CTRL_UP }, { "down", SCE_CTRL_DOWN },
        { "left", SCE_CTRL_LEFT }, { "right", SCE_CTRL_RIGHT }, { "l", SCE_CTRL_L1 }, { "r", SCE_CTRL_R1 },
        { "l2", SCE_CTRL_L2 }, { "r2", SCE_CTRL_R2 }, { "l3", SCE_CTRL_L3 }, { "r3", SCE_CTRL_R3 },
        { "start", SCE_CTRL_START }, { "select", SCE_CTRL_SELECT }
    };
    const auto it = buttons.find(name);
    if (it == buttons.end()) {
        LOG_WARN("Input pipe: unknown button '{}'", name);
        return 0;
    }
    return it->second;
}

static void apply_injected_command(CtrlState &state, const std::string &line) {
    std::istringstream words(line);
    std::string command;
    words >> command;
    if (command == "hold") {
        uint32_t buttons = 0;
        for (std::string name; words >> name;)
            if (name != "none")
                buttons |= parse_injected_button(name);
        // the PS Vita's L/R are L1/R1 in the extended (v2) functions
        uint32_t buttons_basic = buttons & ~(SCE_CTRL_L1 | SCE_CTRL_R1);
        if (buttons & SCE_CTRL_L1)
            buttons_basic |= SCE_CTRL_L;
        if (buttons & SCE_CTRL_R1)
            buttons_basic |= SCE_CTRL_R;
        std::lock_guard<std::mutex> guard(state.mutex);
        state.injected_state.buttons = buttons_basic;
        state.injected_state.buttons_ext = buttons;
    } else if (command == "axes") {
        float axes[4] = {};
        for (float &axis : axes)
            words >> axis;
        std::lock_guard<std::mutex> guard(state.mutex);
        std::copy(std::begin(axes), std::end(axes), state.injected_state.axes);
    } else if (command == "touch") {
        // touch <x> <y> [<x> <y>...] holds front-touch fingers at screen points; "touch none" lifts them
        std::vector<std::pair<float, float>> points;
        for (float x, y; words >> x >> y;)
            points.emplace_back(x, y);
        std::lock_guard<std::mutex> guard(state.mutex);
        state.injected_touches = std::move(points);
    } else if (command == "record") {
        int count = 0;
        std::string dir;
        words >> count;
        std::getline(words >> std::ws, dir);
        std::lock_guard<std::mutex> guard(state.mutex);
        state.injected_records.emplace_back(count, dir);
    } else if (command == "memdump") {
        uint32_t address = 0, size = 0;
        std::string path;
        words >> std::hex >> address >> size >> std::dec;
        std::getline(words >> std::ws, path);
        std::lock_guard<std::mutex> guard(state.mutex);
        state.injected_memdumps.emplace_back(path, address, size);
    } else if (command == "screenshot") {
        std::string path;
        std::getline(words >> std::ws, path);
        std::lock_guard<std::mutex> guard(state.mutex);
        state.injected_screenshots.push_back(path);
    } else if (!command.empty()) {
        LOG_WARN("Input pipe: unknown command '{}'", line);
    }
}

static void input_pipe_thread(CtrlState &state, std::string pipe_name) {
    for (;;) {
        const HANDLE pipe = CreateNamedPipeA(pipe_name.c_str(), PIPE_ACCESS_INBOUND,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 0, 4096, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            LOG_ERROR("Input pipe: could not create {} (error {})", pipe_name, GetLastError());
            return;
        }
        if (ConnectNamedPipe(pipe, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED) {
            std::string pending;
            char buffer[512];
            DWORD read = 0;
            while (ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
                pending.append(buffer, read);
                size_t end;
                while ((end = pending.find('\n')) != std::string::npos) {
                    std::string line = pending.substr(0, end);
                    if (!line.empty() && line.back() == '\r')
                        line.pop_back();
                    apply_injected_command(state, line);
                    pending.erase(0, end + 1);
                }
            }
        }
        CloseHandle(pipe);
    }
}

static void start_input_pipe(CtrlState &state) {
    static std::once_flag started;
    std::call_once(started, [&state]() {
        const char *name = getenv("VITA3K_INPUT_PIPE");
        if (!name || !*name)
            return;
        const std::string pipe_name = fmt::format("\\\\.\\pipe\\{}", name);
        LOG_INFO("Input pipe: listening on {}", pipe_name);
        std::thread(input_pipe_thread, std::ref(state), pipe_name).detach();
    });
}
#endif

static void apply_injected(uint32_t *buttons, float axes[4], bool ext, const CtrlState &state) {
    const auto &injected = state.injected_state;
    *buttons |= ext ? injected.buttons_ext : injected.buttons;
    for (int i = 0; i < 4; i++)
        axes[i] += injected.axes[i];
}

static void apply_keyboard(uint32_t *buttons, float axes[4], bool ext, EmuEnvState &emuenv) {
    const auto &kb = emuenv.ctrl.keyboard_state;
    const uint32_t kb_buttons = ext ? kb.buttons_ext : kb.buttons;

    *buttons |= kb_buttons;
    axes[0] += kb.axes[0];
    axes[1] += kb.axes[1];
    axes[2] += kb.axes[2];
    axes[3] += kb.axes[3];
}

static std::array<ControllerBinding, 13> get_controller_bindings(EmuEnvState &emuenv) {
    return { {
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_BACK]), SCE_CTRL_SELECT },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_START]), SCE_CTRL_START },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_DPAD_UP]), SCE_CTRL_UP },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_DPAD_RIGHT]), SCE_CTRL_RIGHT },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_DPAD_DOWN]), SCE_CTRL_DOWN },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_DPAD_LEFT]), SCE_CTRL_LEFT },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER]), SCE_CTRL_L },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER]), SCE_CTRL_R },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_NORTH]), SCE_CTRL_TRIANGLE },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_EAST]), SCE_CTRL_CIRCLE },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_SOUTH]), SCE_CTRL_CROSS },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_WEST]), SCE_CTRL_SQUARE },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_GUIDE]), SCE_CTRL_PSBUTTON },
    } };
}

std::array<ControllerBinding, 15> get_controller_bindings_ext(EmuEnvState &emuenv) {
    return { {
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_BACK]), SCE_CTRL_SELECT },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_LEFT_STICK]), SCE_CTRL_L3 },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_RIGHT_STICK]), SCE_CTRL_R3 },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_START]), SCE_CTRL_START },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_DPAD_UP]), SCE_CTRL_UP },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_DPAD_RIGHT]), SCE_CTRL_RIGHT },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_DPAD_DOWN]), SCE_CTRL_DOWN },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_DPAD_LEFT]), SCE_CTRL_LEFT },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_LEFT_SHOULDER]), SCE_CTRL_L1 },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER]), SCE_CTRL_R1 },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_NORTH]), SCE_CTRL_TRIANGLE },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_EAST]), SCE_CTRL_CIRCLE },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_SOUTH]), SCE_CTRL_CROSS },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_WEST]), SCE_CTRL_SQUARE },
        { static_cast<SDL_GamepadButton>(emuenv.cfg.controller_binds[SDL_GAMEPAD_BUTTON_GUIDE]), SCE_CTRL_PSBUTTON },
    } };
}

static float axis_to_axis(int16_t axis, const auto &mult) {
    const auto unsigned_axis = static_cast<float>(axis - INT16_MIN);
    assert(unsigned_axis >= 0);
    assert(unsigned_axis <= UINT16_MAX);

    const auto f = unsigned_axis / UINT16_MAX;

    const auto output = std::clamp(((f * 2) - 1) * mult, -1.0f, 1.f);

    return output;
}

static uint8_t float_to_byte(float f) {
    const auto clamped_f = std::clamp(f, -1.0f, 1.0f);
    const auto mapped = (clamped_f * 0.5f) + 0.5f;

    return static_cast<uint8_t>(mapped * 255);
}

static void apply_controller(EmuEnvState &emuenv, uint32_t *buttons, float axes[4], SDL_Gamepad *controller, bool ext) {
    const auto &axis_binds = emuenv.cfg.controller_axis_binds;

    if (ext) {
        for (const auto &binding : get_controller_bindings_ext(emuenv)) {
            if (SDL_GetGamepadButton(controller, binding.controller)) {
                *buttons |= binding.button;
            }
        }

        if (SDL_GetGamepadAxis(controller, static_cast<SDL_GamepadAxis>(axis_binds[4])) > 0x3FFF) {
            *buttons |= SCE_CTRL_L2;
        }
        if (SDL_GetGamepadAxis(controller, static_cast<SDL_GamepadAxis>(axis_binds[5])) > 0x3FFF) {
            *buttons |= SCE_CTRL_R2;
        }
    } else {
        for (const auto &binding : get_controller_bindings(emuenv)) {
            if (SDL_GetGamepadButton(controller, binding.controller)) {
                *buttons |= binding.button;
            }
        }
    }

    auto &analog_multiplier = emuenv.cfg.controller_analog_multiplier;
    axes[0] += axis_to_axis(SDL_GetGamepadAxis(controller, static_cast<SDL_GamepadAxis>(axis_binds[0])), analog_multiplier);
    axes[1] += axis_to_axis(SDL_GetGamepadAxis(controller, static_cast<SDL_GamepadAxis>(axis_binds[1])), analog_multiplier);
    axes[2] += axis_to_axis(SDL_GetGamepadAxis(controller, static_cast<SDL_GamepadAxis>(axis_binds[2])), analog_multiplier);
    axes[3] += axis_to_axis(SDL_GetGamepadAxis(controller, static_cast<SDL_GamepadAxis>(axis_binds[3])), analog_multiplier);
}

static void retrieve_ctrl_data(EmuEnvState &emuenv, int port, bool is_v2, bool negative, bool from_ext_function, SceUInt32 &buttons, SceUInt8 &lx, SceUInt8 &ly, SceUInt8 &rx, SceUInt8 &ry) {
#ifdef _WIN32
    start_input_pipe(emuenv.ctrl);
#endif
    std::lock_guard<std::mutex> guard(emuenv.ctrl.mutex);

    if (port == 0) {
        port++;
    }
    CtrlState &state = emuenv.ctrl;

    std::array<float, 4> axes{};
    axes.fill(0);

    const auto reset_axes = [&]() {
        // Re-center joysticks to (128,128). Range is (0-255,0-255).
        SceCtrlPadInputMode mode = from_ext_function ? state.input_mode_ext : state.input_mode;
        if (mode == SCE_CTRL_MODE_DIGITAL) {
            lx = 0x80;
            ly = 0x80;
            rx = 0x80;
            ry = 0x80;
        } else {
            lx = float_to_byte(axes[0]);
            ly = float_to_byte(axes[1]);
            rx = float_to_byte(axes[2]);
            ry = float_to_byte(axes[3]);
        }
        if (negative)
            buttons ^= ~0;
    };

    if (state.overlay_input_intercepted.load(std::memory_order_relaxed) || emuenv.drop_inputs) {
        reset_axes();
        return;
    }

    if (emuenv.cfg.current_config.pstv_mode) {
        if (port == 1) {
            apply_keyboard(&buttons, axes.data(), is_v2, emuenv);
        }
        for (const auto &[_, controller] : state.controllers) {
            if (controller.port + 1 == port) {
                // sceCtrl ports are 1-based and SDL_GameController index is 0-based. Need to convert.
                apply_controller(emuenv, &buttons, axes.data(), controller.controller.get(), is_v2);
            }
        }
    } else if (port == 1) {
        // If not in PSTV mode, every controller input is considered as a port 1 input
        apply_keyboard(&buttons, axes.data(), is_v2, emuenv);
        apply_injected(&buttons, axes.data(), is_v2, state);
        for (const auto &[_, controller] : state.controllers) {
            apply_controller(emuenv, &buttons, axes.data(), controller.controller.get(), is_v2);
        }
    }

    reset_axes();
}

int ctrl_get(const SceUID thread_id, EmuEnvState &emuenv, int port, SceCtrlData2 *pData, SceUInt32 count, bool negative, bool is_peek, bool is_v2, bool from_ext) {
    if (port > 1 && !emuenv.cfg.current_config.pstv_mode) {
        const char *export_name = "sceCtrl*Buffer*";
        return RET_ERROR(SCE_CTRL_ERROR_NO_DEVICE);
    }

    memset(pData, 0, sizeof(SceCtrlData2) * count);

    CtrlState &state = emuenv.ctrl;

    int nb_returned_data = 1;
    if (is_peek) {
        nb_returned_data = count;
    } else {
        const uint64_t current_vcount = emuenv.display.vblank_count.load();
        if (current_vcount <= state.last_vcount[port]) {
            // sceCtrlRead is blocking, wait for the next vsync for the buffer to be updated
            auto thread = emuenv.kernel.get_thread(thread_id);

            wait_vblank(emuenv.display, thread, state.last_vcount[port] + 1, false);
        }
        uint64_t vblank_count = emuenv.display.vblank_count.load();
        nb_returned_data = std::min<int32_t>(count, vblank_count - state.last_vcount[port]);
        state.last_vcount[port] = vblank_count;
    }

    std::chrono::time_point<std::chrono::steady_clock> ts = std::chrono::steady_clock::now();
    uint64_t timestamp = std::chrono::duration_cast<std::chrono::microseconds>(ts.time_since_epoch()).count();
    pData->timeStamp = timestamp;
    retrieve_ctrl_data(emuenv, port, is_v2, negative, from_ext, pData->buttons, pData->lx, pData->ly, pData->rx, pData->ry);

    for (int i = 1; i < nb_returned_data; i++) {
        memcpy(&pData[i], &pData[0], sizeof(SceCtrlData2));

        // update the timestamp, 1 vsync = 1/60 sec = 16 667 us earlier
        pData[i].timeStamp -= i * 16667ULL;
    }

    return nb_returned_data;
}
