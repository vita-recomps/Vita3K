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

#include <audio/state.h>

#include <audio/impl/cubeb_audio.h>
#include <audio/impl/sdl_audio.h>

#include <util/log.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace {

// used when the audio backend can't start at all (the SDL backend falls back to SDL's dummy driver before
// that): the ports take the sound at the pace a device would and drop it, so a game runs silent instead
// of stopping
struct SilentAudioOutPort : AudioOutPort {
    // when the sound given so far would have finished playing
    std::chrono::steady_clock::time_point end{};
};

class SilentAudioAdapter : public AudioAdapter {
    // as the SDL adapter: up to 2048 samples (43 ms at 48 kHz) may wait in a port's queue
    static constexpr int queue_samples = 2048;

    static std::chrono::microseconds queued(const SilentAudioOutPort &port) {
        const auto now = std::chrono::steady_clock::now();
        return port.end > now ? std::chrono::duration_cast<std::chrono::microseconds>(port.end - now) : std::chrono::microseconds(0);
    }

public:
    explicit SilentAudioAdapter(AudioState &audio_state)
        : AudioAdapter(audio_state) {}

    bool init() override {
        return true;
    }

    AudioOutPortPtr open_port(int nb_channels, int freq, int nb_sample) override {
        auto port = std::make_shared<SilentAudioOutPort>();
        port->freq = freq;
        port->len_bytes = nb_sample * nb_channels * static_cast<int>(sizeof(int16_t));
        port->len_microseconds = (nb_sample * 1'000'000ULL) / freq;
        return port;
    }

    // the sound is queued and dropped: the call waits only while the queue is full, as a device's would
    void audio_output(AudioOutPort &out_port, const void *buffer) override {
        auto &port = static_cast<SilentAudioOutPort &>(out_port);
        const auto limit = std::chrono::microseconds(queue_samples * 1'000'000LL / port.freq);
        if (queued(port) > limit)
            std::this_thread::sleep_until(port.end - limit);
        port.end = std::max(port.end, std::chrono::steady_clock::now()) + std::chrono::microseconds(port.len_microseconds);
    }

    int get_rest_sample(AudioOutPort &out_port) override {
        auto &port = static_cast<SilentAudioOutPort &>(out_port);
        return static_cast<int>(queued(port).count() * port.freq / 1'000'000);
    }
};

} // namespace

bool AudioState::init(const std::string &adapter_name) {
    set_backend(adapter_name);
    if (!adapter)
        return false;

    return true;
}

void AudioState::stop_all_ports() {
    wake_all_ports();
}

void AudioState::deinit() {
    stop_all_ports();

    const std::lock_guard<std::mutex> lock(mutex);

    out_ports.clear();
    muted = false;
    if (suspended && adapter)
        adapter->switch_state(false);
    suspended = false;

    if (in_port.running) {
        SDL_DestroyAudioStream(static_cast<SDL_AudioStream *>(in_port.id));
        in_port.id = nullptr;
        in_port.running = false;
        in_port.len_bytes = 0;
    }

    next_port_id = 1;
}

void AudioState::set_backend(const std::string &adapter_name) {
    if (adapter_name == this->audio_backend)
        return;

    // first delete all ports then delete the backend
    out_ports.clear();
    adapter.reset();
    if (adapter_name == "SDL") {
        adapter = std::make_unique<SDLAudioAdapter>(*this);
    } else if (adapter_name == "Cubeb") {
        adapter = std::make_unique<CubebAudioAdapter>(*this);
    } else {
        LOG_ERROR("Unknown audio adapter {}", adapter_name);
        return;
    }
    this->audio_backend = adapter_name;

    // lock the mutex to make sure nothing happens until the initialisation is done
    const std::lock_guard<std::mutex> lock(mutex);
    if (!adapter->init()) {
        LOG_WARN("No audio output: the game runs without sound");
        adapter = std::make_unique<SilentAudioAdapter>(*this);
        adapter->init();
    }
    if (suspended)
        adapter->switch_state(true);
}

AudioOutPortPtr AudioState::open_port(int nb_channels, int freq, int nb_sample) {
    return adapter->open_port(nb_channels, freq, nb_sample);
}

int AudioState::register_port(const AudioOutPortPtr &port, int id) {
    const std::lock_guard lock(mutex);
    apply_volume(*port);
    if (id == 0)
        id = next_port_id++;
    out_ports.emplace(id, port);
    return id;
}

// Loudness log for sound tests on devices whose output can't be recorded (a phone): each output
// port's level over every second, as the game wrote it (before the volume setting), in the log.
// On with the environment variable VITA3K_AUDIO_LEVELS=1, or on Android the system property
// debug.vitarecomps.audio_levels=1 (adb shell setprop; read when the game starts).
static bool audio_levels_enabled() {
    static const bool enabled = [] {
#ifdef __ANDROID__
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.vitarecomps.audio_levels", value) > 0 && value[0] == '1')
            return true;
#endif
        const char *env = std::getenv("VITA3K_AUDIO_LEVELS");
        return env && env[0] == '1';
    }();
    return enabled;
}

static void log_audio_level(AudioOutPort &port, const void *buffer) {
    const auto *samples = static_cast<const int16_t *>(buffer);
    const int count = port.len_bytes / 2;
    for (int i = 0; i < count; i++) {
        const int s = samples[i];
        port.level_squares += double(s) * s;
        port.level_peak = std::max(port.level_peak, s < 0 ? -s : s);
    }
    port.level_samples += count;
    const uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (port.level_start == 0)
        port.level_start = now;
    if (now - port.level_start < 1000)
        return;
    const double rms = port.level_samples ? std::sqrt(port.level_squares / port.level_samples) / 32768.0 : 0;
    LOG_INFO("audio level: port {} ({} Hz, mode {}) rms {:.1f} dB peak {:.1f} dB", fmt::ptr(&port), port.freq, port.mode,
        20 * std::log10(rms + 1e-9), 20 * std::log10(port.level_peak / 32768.0 + 1e-9));
    port.level_squares = 0;
    port.level_peak = 0;
    port.level_samples = 0;
    port.level_start = now;
}

void AudioState::audio_output(AudioOutPort &out_port, const void *buffer) {
    if (out_port.stopping)
        return;

    if (audio_levels_enabled())
        log_audio_level(out_port, buffer);

    adapter->audio_output(out_port, buffer);

    if (out_port.stopping)
        return;

    uint64_t now = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    uint64_t diff = now - out_port.last_output;
    uint64_t to_wait = out_port.len_microseconds - diff;
    if (diff < out_port.len_microseconds && to_wait > 1000) {
        // This is what we should be waiting to be perfectly accurate
        // However, doing so would cause the host audio buffer to often lack samples to output
        // This is because the PS Vita and the host audio parameters do not match exactly
        // So instead only wait 50% of the time
        // also don't sleep for less than 0.5 ms
        to_wait /= 2;
        std::this_thread::sleep_for(std::chrono::microseconds(to_wait));
        out_port.last_output = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    } else {
        out_port.last_output = now;
    }
}

void AudioState::set_volume(AudioOutPort &out_port, float volume) {
    const std::lock_guard lock(mutex);
    out_port.volume = volume;
    apply_volume(out_port);
}

void AudioState::apply_volume(AudioOutPort &port) {
    adapter->set_volume(port, muted ? 0.0f : port.volume * global_volume);
}

void AudioState::set_global_volume(float volume) {
    const std::lock_guard lock(mutex);
    global_volume = volume;
    for (const auto &[_, port] : out_ports)
        apply_volume(*port);
}

void AudioState::set_pause_state(bool paused, bool freeze) {
    const std::lock_guard lock(mutex);
    if (suspended != freeze) {
        adapter->switch_state(freeze);
        suspended = freeze;
    }
    // A running Ad Hoc session must keep draining each audio stream at its normal rate.
    if (muted != paused) {
        muted = paused;
        for (const auto &[_, port] : out_ports)
            apply_volume(*port);
    }
}

int AudioState::get_rest_sample(AudioOutPort &out_port) {
    return adapter->get_rest_sample(out_port);
}

void AudioState::wake_all_ports() {
    const std::lock_guard<std::mutex> lock(mutex);
    for (auto &[_, port] : out_ports) {
        port->stopping = true;
    }
    if (adapter)
        adapter->wake_all_ports();
}
