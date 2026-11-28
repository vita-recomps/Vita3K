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

// The single-game libraries' glue to their APK's Java side on Android (ss-delta 09-android), in place
// of Vita3K's own Android app (android/jni), which the games don't build:
//   * the surface: the renderer draws only while the Java SurfaceView has one. The Java class
//     io.github.vitarecomps.frontend.GameSurface (game-frontend/android/java) reports it; every
//     game's APK compiles that class, so the JNI name below is the same for all of them;
//   * the soft keyboard the game's text dialogs ask for (ime/keyboard.h): SDL's text input;
//   * the on-screen controls (io.github.vitarecomps.frontend.OnScreenControls): what they hold goes
//     into the controller state the game reads (CtrlState::screen_state), their menu button opens the
//     settings menu, and the frontend tells them [controls], the game's touch spots and when the
//     settings menu is open.

#ifdef __ANDROID__

#include <app/session_controller.h>
#include <ctrl/ctrl.h>
#include <ctrl/state.h>
#include <emuenv/state.h>
#include <ime/keyboard.h>
#include <renderer/vulkan/screen_renderer.h>
#include <util/android_driver.h>
#include <util/fs.h>
#include <util/log.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_keyboard.h>
#include <SDL3/SDL_system.h>
#include <miniz.h>
#include <spdlog/spdlog.h>

#include <jni.h>

#include <fstream>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace {
SDL_Window *s_window = nullptr;

// the running game's state, for the on-screen controls' calls from Java's UI thread (null before the
// game's window exists and once the frontend leaves)
std::mutex s_emuenv_mutex;
EmuEnvState *s_emuenv = nullptr;
// the running game's session, for the pause in the background (guarded by s_emuenv_mutex; null
// outside sdl_frontend::run)
app::AppSessionController *s_session = nullptr;

// The game pauses while the app is in the background, as behind the settings menu (the session's
// Background reason; during Ad Hoc Play it keeps running, session_controller.cpp). Without it the
// game went on behind the home screen with no surface to draw on, at more CPU than in front (Thor,
// ss-delta #29).
void pause_in_background(bool background) {
    const std::lock_guard<std::mutex> lock(s_emuenv_mutex);
    if (s_session && s_session->set_pause_reason(app::AppSessionPauseReason::Background, background))
        LOG_INFO("Android: the app {} the screen", background ? "left" : "is back on");
}

// Android may kill a paused app without another word (swiped away from the recent apps): the log
// is written on a thread of its own, so its last lines are flushed when the app leaves the screen.
// An event watch, because SDL blocks the game's event loop while the app is paused.
bool SDLCALL flush_logs_on_pause(void *, SDL_Event *event) {
    switch (event->type) {
    case SDL_EVENT_WILL_ENTER_BACKGROUND:
        pause_in_background(true);
        [[fallthrough]];
    case SDL_EVENT_DID_ENTER_BACKGROUND:
    case SDL_EVENT_TERMINATING:
    case SDL_EVENT_LOW_MEMORY:
        spdlog::apply_all([](const std::shared_ptr<spdlog::logger> &logger) { logger->flush(); });
        break;
    case SDL_EVENT_DID_ENTER_FOREGROUND:
        pause_in_background(false);
        break;
    default:
        break;
    }
    return true;
}
} // namespace

namespace game_frontend {

// Vulkan driver packages (libadrenotools zips, such as Turnip's <name>.adpkg.zip) that a PC put in
// <game_dir>/drivers/: unpacked into the app's internal files/driver/<name>/, where the driver loader
// takes them from ([debug] gpu_driver = <name>), when the zip is new or changed. A release APK can't
// be reached there from a PC (no run-as), so this is how a driver gets in (ss-delta 09-android).
void android_install_driver_packages(const fs::path &game_dir) {
    const fs::path source_dir = game_dir / "drivers";
    if (!fs::is_directory(source_dir))
        return;
    const fs::path driver_root = fs::path(SDL_GetAndroidInternalStoragePath()) / "driver";
    boost::system::error_code ec;
    for (const auto &entry : fs::directory_iterator(source_dir)) {
        const fs::path zip_path = entry.path();
        if (zip_path.extension() != ".zip")
            continue;
        std::string name = zip_path.stem().string(); // <name>.adpkg.zip -> <name>.adpkg -> <name>
        if (name.ends_with(".adpkg"))
            name.resize(name.size() - 6);
        const fs::path target = driver_root / name;
        const std::string stamp = fmt::format("{} {}", fs::file_size(zip_path), fs::last_write_time(zip_path));
        const fs::path marker = target / ".package";
        if (std::ifstream in{ fs_utils::path_to_utf8(marker) }; in) {
            std::stringstream recorded;
            recorded << in.rdbuf();
            if (recorded.str() == stamp)
                continue;
        }
        fs::remove_all(target, ec);
        fs::create_directories(target, ec);

        mz_zip_archive zip{};
        if (!mz_zip_reader_init_file(&zip, fs_utils::path_to_utf8(zip_path).c_str(), 0)) {
            LOG_ERROR("Driver package {}: not a zip ({})", zip_path, mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
            continue;
        }
        bool ok = true;
        for (mz_uint i = 0; ok && i < mz_zip_reader_get_num_files(&zip); i++) {
            mz_zip_archive_file_stat stat{};
            if (!mz_zip_reader_file_stat(&zip, i, &stat) || mz_zip_reader_is_file_a_directory(&zip, i))
                continue;
            const fs::path relative = fs::path(stat.m_filename).lexically_normal();
            if (!android_driver::is_safe_driver_relative_path(relative)) {
                LOG_ERROR("Driver package {}: unsafe entry {}", zip_path, stat.m_filename);
                ok = false;
                break;
            }
            const fs::path out = target / relative;
            fs::create_directories(out.parent_path(), ec);
            ok = mz_zip_reader_extract_to_file(&zip, i, fs_utils::path_to_utf8(out).c_str(), 0);
        }
        mz_zip_reader_end(&zip);
        if (!ok || !android_driver::resolve_custom_driver_library_name(target)) {
            LOG_ERROR("Driver package {} could not be unpacked into {}", zip_path, target);
            fs::remove_all(target, ec);
            continue;
        }
        std::ofstream(fs_utils::path_to_utf8(marker)) << stamp;
        LOG_INFO("Driver package {} unpacked: [debug] gpu_driver = {} uses it", zip_path.filename(), name);
    }
}

// The driver packages on the device, by the name [debug] gpu_driver takes: those unpacked into
// files/driver/ and those still waiting as zips in the game's drivers/ (unpacked at the next start)
std::vector<std::string> android_driver_names() {
    std::set<std::string> names;
    boost::system::error_code ec;
    const fs::path driver_root = fs::path(SDL_GetAndroidInternalStoragePath()) / "driver";
    if (fs::is_directory(driver_root, ec))
        for (const auto &entry : fs::directory_iterator(driver_root, ec))
            if (fs::exists(entry.path() / ".package", ec))
                names.insert(entry.path().filename().string());
    if (const char *external = SDL_GetAndroidExternalStoragePath()) {
        const fs::path drivers = fs::path(external) / "drivers";
        if (fs::is_directory(drivers, ec))
            for (const auto &entry : fs::directory_iterator(drivers, ec)) {
                if (entry.path().extension() != ".zip")
                    continue;
                std::string name = entry.path().stem().string();
                if (name.ends_with(".adpkg"))
                    name.resize(name.size() - 6);
                names.insert(name);
            }
    }
    return { names.begin(), names.end() };
}

// [debug] gpu_driver as Vita3K's custom driver name: auto is the package installed last from drivers/
// (the newest marker), or the system's driver when there is none; system (or empty) is the system's
std::string android_gpu_driver(const std::string &setting) {
    if (setting.empty() || setting == "system")
        return {};
    if (setting != "auto")
        return setting;
    const fs::path driver_root = fs::path(SDL_GetAndroidInternalStoragePath()) / "driver";
    std::string newest;
    std::time_t newest_time = 0;
    boost::system::error_code ec;
    if (fs::is_directory(driver_root)) {
        for (const auto &entry : fs::directory_iterator(driver_root)) {
            const fs::path marker = entry.path() / ".package";
            if (!fs::exists(marker))
                continue;
            const std::time_t t = fs::last_write_time(marker, ec);
            if (!ec && (newest.empty() || t > newest_time)) {
                newest = entry.path().filename().string();
                newest_time = t;
            }
        }
    }
    return newest;
}

// the frontend's window exists: the soft keyboard's window, the log flush on pause
void android_window_created(SDL_Window *window) {
    ime::set_sdl_window(window);
    static bool watching = false;
    if (!watching) {
        SDL_AddEventWatch(flush_logs_on_pause, nullptr);
        watching = true;
    }
}

// the game's state for the on-screen controls (null: the game is gone)
void android_set_emuenv(EmuEnvState *emuenv) {
    const std::lock_guard<std::mutex> lock(s_emuenv_mutex);
    s_emuenv = emuenv;
}

// the game's session, for the pause in the background (null: it is gone)
void android_set_session(app::AppSessionController *session) {
    const std::lock_guard<std::mutex> lock(s_emuenv_mutex);
    s_session = session;
}

// Calls a method of GameActivity, `call(env, activity, method)`, from the game's thread. The setup's
// activities run this library too, without GameActivity's methods: nothing happens there.
template <typename Call>
void call_game_activity(const char *name, const char *signature, Call &&call) {
    JNIEnv *env = static_cast<JNIEnv *>(SDL_GetAndroidJNIEnv());
    jobject activity = static_cast<jobject>(SDL_GetAndroidActivity()); // a local reference
    if (!env || !activity)
        return;
    jclass cls = env->GetObjectClass(activity);
    if (jmethodID method = env->GetMethodID(cls, name, signature))
        call(env, activity, method);
    if (env->ExceptionCheck())
        env->ExceptionClear(); // NoSuchMethodError outside GameActivity
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
}

// The on-screen controls' settings ([controls]), the game's touch spots (960x544 points) they leave
// clear and its latch button (Profile::screen_latch: the Vita button, its label; none when empty):
// GameActivity.configureOnScreenControls, which shows or hides them on the UI thread
void android_configure_screen_controls(bool enabled, int size, int opacity, const std::vector<std::pair<float, float>> &spots,
    uint32_t latch_button, const std::string &latch_label) {
    call_game_activity("configureOnScreenControls", "(ZII[FILjava/lang/String;)V", [&](JNIEnv *env, jobject activity, jmethodID method) {
        std::vector<jfloat> flat;
        for (const auto &[x, y] : spots) {
            flat.push_back(x);
            flat.push_back(y);
        }
        jfloatArray points = env->NewFloatArray(static_cast<jsize>(flat.size()));
        if (!points)
            return;
        env->SetFloatArrayRegion(points, 0, static_cast<jsize>(flat.size()), flat.data());
        jstring label = env->NewStringUTF(latch_label.c_str());
        env->CallVoidMethod(activity, method, static_cast<jboolean>(enabled), static_cast<jint>(size), static_cast<jint>(opacity), points,
            static_cast<jint>(latch_button), label);
        if (label)
            env->DeleteLocalRef(label);
        env->DeleteLocalRef(points);
    });
}

// The settings menu opened or closed: the on-screen controls show while it is open even when they are
// off, so that a phone without a gamepad can always change them back
void android_settings_menu_open(bool open) {
    call_game_activity("onSettingsMenu", "(Z)V", [&](JNIEnv *env, jobject activity, jmethodID method) {
        env->CallVoidMethod(activity, method, static_cast<jboolean>(open));
    });
}

} // namespace game_frontend

extern "C" JNIEXPORT void JNICALL
Java_io_github_vitarecomps_frontend_GameSurface_setSurfaceStatus(JNIEnv *, jobject, jboolean surface_present) {
    renderer::vulkan::set_android_surface(surface_present);
}

// The on-screen controls' buttons (SceCtrlButtons as the extended functions read them: L1, R1) and
// sticks (-1 to 1, down and right positive), whenever they change
extern "C" JNIEXPORT void JNICALL
Java_io_github_vitarecomps_frontend_OnScreenControls_setState(JNIEnv *, jclass, jint buttons, jfloat lx, jfloat ly, jfloat rx, jfloat ry) {
    const std::lock_guard<std::mutex> lock(s_emuenv_mutex);
    if (!s_emuenv)
        return;
    const uint32_t ext = static_cast<uint32_t>(buttons);
    // the basic functions' L and R are the extended ones' L1 and R1
    uint32_t basic = ext & ~static_cast<uint32_t>(SCE_CTRL_L1 | SCE_CTRL_R1);
    if (ext & SCE_CTRL_L1)
        basic |= SCE_CTRL_L;
    if (ext & SCE_CTRL_R1)
        basic |= SCE_CTRL_R;
    const std::lock_guard<std::mutex> ctrl_lock(s_emuenv->ctrl.mutex);
    auto &state = s_emuenv->ctrl.screen_state;
    state.buttons = basic;
    state.buttons_ext = ext;
    state.axes[0] = lx;
    state.axes[1] = ly;
    state.axes[2] = rx;
    state.axes[3] = ry;
}

// the on-screen menu button: the settings menu opens, or closes (as F1 or a held Select)
extern "C" JNIEXPORT void JNICALL
Java_io_github_vitarecomps_frontend_OnScreenControls_toggleMenu(JNIEnv *, jclass) {
    const std::lock_guard<std::mutex> lock(s_emuenv_mutex);
    if (!s_emuenv)
        return;
    const std::lock_guard<std::mutex> ctrl_lock(s_emuenv->ctrl.mutex);
    s_emuenv->ctrl.injected_menu_toggle = true;
}

namespace game_frontend {
void android_menu_action(const std::string &action) {
    call_game_activity("onMenuAction", "(Ljava/lang/String;)V", [&](JNIEnv *env, jobject activity, jmethodID method) {
        jstring name = env->NewStringUTF(action.c_str());
        env->CallVoidMethod(activity, method, name);
        env->DeleteLocalRef(name);
    });
}
}

namespace ime {

void set_sdl_window(SDL_Window *window) {
    s_window = window;
}

void set_keyboard_active(bool active) {
    if (!s_window)
        return;
    if (active)
        SDL_StartTextInput(s_window);
    else
        SDL_StopTextInput(s_window);
}

void notify_ime_state_changed() {
}

} // namespace ime

#endif // __ANDROID__
