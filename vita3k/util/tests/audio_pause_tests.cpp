// SPDX-License-Identifier: GPL-2.0-or-later
#include <audio/state.h>

#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <vector>

class TestAudioAdapter : public AudioAdapter {
public:
    using AudioAdapter::AudioAdapter;
    std::unordered_map<const AudioOutPort *, float> gains;
    std::vector<bool> suspensions;

    bool init() override { return true; }
    AudioOutPortPtr open_port(int, int, int) override { return std::make_shared<AudioOutPort>(); }
    void set_volume(AudioOutPort &port, float volume) override { gains[&port] = volume; }
    void switch_state(bool suspend) override { suspensions.push_back(suspend); }
};

static void require(bool ok, const char *why) {
    if (!ok)
        throw std::runtime_error(why);
}

int main() {
    AudioState state;
    auto adapter = std::make_unique<TestAudioAdapter>(state);
    auto &output = *adapter;
    state.adapter = std::move(adapter);
    auto port = state.open_port(2, 48000, 256);
    state.register_port(port);
    state.set_global_volume(0.5f);
    state.set_volume(*port, 0.5f);
    require(output.gains.at(port.get()) == 0.25f, "initial gain");

    state.set_pause_state(true, false); // Ad Hoc, with guest threads running
    require(output.suspensions.empty(), "Ad Hoc pause suspended the audio device");
    require(output.gains.at(port.get()) == 0.0f, "Ad Hoc pause did not mute the port");
    state.set_volume(*port, 0.25f);
    state.set_global_volume(0.25f);
    auto new_port = state.open_port(1, 48000, 256);
    state.register_port(new_port);
    require(output.gains.at(port.get()) == 0.0f && output.gains.at(new_port.get()) == 0.0f,
        "port created or changed during pause was audible");

    state.set_pause_state(false, false);
    require(output.gains.at(port.get()) == 0.0625f && output.gains.at(new_port.get()) == 0.25f,
        "resume did not restore the latest volume");
    require(output.suspensions.empty(), "Ad Hoc resume restarted a running audio device");

    state.set_pause_state(true, true);
    require(output.suspensions == std::vector{ true }, "offline pause did not suspend audio");
    state.set_pause_state(true, false);
    require(output.suspensions == std::vector{ true, false }, "Ad Hoc must start draining audio again");
    require(output.gains.at(port.get()) == 0.0f, "audio was audible with a pause still active");

    state.set_pause_state(true, true);
    state.deinit();
    require(output.suspensions == std::vector{ true, false, true, false }, "deinit did not release suspended audio");
    state.adapter.reset();
    state.deinit();
}
