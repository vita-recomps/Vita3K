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

#include <ngs/modules/reverb.h>
#include <util/log.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

// The reverb buss: its parameters follow I3DL2 (levels in millibels, times in seconds, diffusion and
// density in percent, references in Hz). The hardware's algorithm is not known; this is a Freeverb-
// style approximation driven by those parameters: early reflections from taps on a delay line, then
// a late reverb of eight damped combs per channel (decay time, HF ratio) into four allpasses
// (diffusion). **Hypothesis** throughout: levels and character are not checked against a Vita.

namespace ngs {

namespace {

constexpr float PI = 3.14159265358979f;

// I3DL2's "Generic". **Hypothesis**: the index and values of the firmware's preset 0 are not known
// (libngs is not decrypted here). fDryMB = -10000: a send buss, so no dry signal passes.
const SceNgsReverbParams PRESETS[] = {
    { { SCE_NGS_REVERB_PARAMS_STRUCT_ID, sizeof(SceNgsReverbParams) }, -1000.0f, -100.0f, 1.49f, 0.83f, -2602.0f, 0.007f, 200.0f, 0.011f, 100.0f, 100.0f, 5000.0f,
        { SCE_NGS_REVERB_ROOM1_LEFT, SCE_NGS_REVERB_ROOM1_RIGHT }, 1.0f, 250.0f, 0.0f, -10000.0f },
};

// Freeverb's tunings at 44.1 kHz, scaled to the system's rate
constexpr std::array<int, 8> COMB_TUNING = { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
constexpr std::array<int, 4> ALLPASS_TUNING = { 556, 441, 341, 225 };
constexpr int STEREO_SPREAD = 23;
constexpr float INPUT_GAIN = 0.015f;
constexpr float WET_SCALE = 3.0f;

// early reflection taps (ms after fReflectionsDelay, gain) per pattern, left and right of room 1;
// rooms 2 and 3 stretch them
constexpr std::array<std::array<float, 2>, 4> EARLY_TAPS_LEFT = { { { 0.0f, 1.0f }, { 7.1f, 0.7f }, { 13.3f, 0.5f }, { 21.7f, 0.35f } } };
constexpr std::array<std::array<float, 2>, 4> EARLY_TAPS_RIGHT = { { { 0.0f, 1.0f }, { 5.3f, 0.7f }, { 11.9f, 0.5f }, { 19.7f, 0.35f } } };

constexpr float MAX_DELAY_S = 0.3f + 0.1f + 0.05f; // reflections + reverb delay maxima, and the taps

float millibels(const float mb) {
    return mb <= -10000.0f ? 0.0f : std::pow(10.0f, mb / 2000.0f);
}

// a one-pole lowpass coefficient whose gain at cos_w is `gain` (OpenAL's derivation, on power)
float lowpass_coefficient(const float gain, const float cos_w) {
    const float g = std::clamp(gain * gain, 0.001f, 1.0f);
    if (g >= 0.9999f)
        return 0.0f;
    const float a = (1.0f - g * cos_w - std::sqrt(std::max(0.0f, 2.0f * g * (1.0f - cos_w) - g * g * (1.0f - cos_w * cos_w)))) / (1.0f - g);
    return std::clamp(a, 0.0f, 0.99f);
}

float flush(const float v) {
    return std::fabs(v) < 1e-20f ? 0.0f : v;
}

struct Comb {
    std::vector<float> buffer;
    size_t pos = 0;
    float store = 0.0f;

    float process(const float input, const float feedback, const float damp) {
        const float out = buffer[pos];
        store = flush(out * (1.0f - damp) + store * damp);
        buffer[pos] = input + store * feedback;
        if (++pos >= buffer.size())
            pos = 0;
        return out;
    }
};

struct Allpass {
    std::vector<float> buffer;
    size_t pos = 0;

    float process(const float input, const float feedback) {
        const float delayed = buffer[pos];
        buffer[pos] = flush(input + delayed * feedback);
        if (++pos >= buffer.size())
            pos = 0;
        return delayed - input;
    }
};

struct ReverbRuntimeState : public ModuleRuntimeState {
    int32_t sample_rate = 0;
    std::array<std::array<Comb, 8>, 2> combs;
    std::array<std::array<Allpass, 4>, 2> allpasses;
    std::vector<float> delay; // stereo frames of the input, for the early taps and the late predelay
    size_t delay_pos = 0;
    std::array<float, 2> hf_state = { 0.0f, 0.0f };

    void init(const int32_t rate) {
        sample_rate = rate;
        const float scale = static_cast<float>(rate) / 44100.0f;
        for (int ch = 0; ch < 2; ch++) {
            for (size_t i = 0; i < COMB_TUNING.size(); i++)
                combs[ch][i] = { std::vector<float>(std::max(1, static_cast<int>((COMB_TUNING[i] + ch * STEREO_SPREAD) * scale)), 0.0f) };
            for (size_t i = 0; i < ALLPASS_TUNING.size(); i++)
                allpasses[ch][i] = { std::vector<float>(std::max(1, static_cast<int>((ALLPASS_TUNING[i] + ch * STEREO_SPREAD) * scale)), 0.0f) };
        }
        delay.assign(static_cast<size_t>(MAX_DELAY_S * rate + 1) * 2, 0.0f);
        delay_pos = 0;
        hf_state = { 0.0f, 0.0f };
    }

    void clear() {
        for (auto &channel : combs)
            for (Comb &c : channel) {
                std::fill(c.buffer.begin(), c.buffer.end(), 0.0f);
                c.store = 0.0f;
            }
        for (auto &channel : allpasses)
            for (Allpass &a : channel)
                std::fill(a.buffer.begin(), a.buffer.end(), 0.0f);
        std::fill(delay.begin(), delay.end(), 0.0f);
        hf_state = { 0.0f, 0.0f };
    }

    // the input `frames` frames ago, channel ch
    float delayed(const size_t frames, const int ch) const {
        const size_t length = delay.size() / 2;
        const size_t back = std::min(frames, length - 1);
        return delay[((delay_pos + length - back) % length) * 2 + ch];
    }
};

} // namespace

uint32_t reverb_preset_count() {
    return static_cast<uint32_t>(std::size(PRESETS));
}

bool get_reverb_preset(const uint32_t index, SceNgsReverbParams &params) {
    if (index >= reverb_preset_count())
        return false;
    params = PRESETS[index];
    return true;
}

std::unique_ptr<ModuleRuntimeState> ReverbModule::create_runtime_state() const {
    return std::make_unique<ReverbRuntimeState>();
}

void ReverbModule::on_state_change(const MemState &mem, ModuleData &data, const VoiceState previous) {
    // a new play starts without the old tail
    if (data.parent->state == VOICE_STATE_ACTIVE && previous == VOICE_STATE_AVAILABLE)
        data.get_runtime_state<ReverbRuntimeState>()->clear();
}

bool ReverbModule::process(KernelState &kern, const MemState &mem, const SceUID thread_id, ModuleData &data, std::unique_lock<std::recursive_mutex> &scheduler_lock, std::unique_lock<std::mutex> &voice_lock) {
    if (data.is_bypassed)
        return false;
    if (!data.parent->rack->system->reverb) {
        // the reverb is off for this game (ngs::State::Effects): the buss passes its input on
        const SceNgsParamsDescriptor *desc = data.get_parameters<SceNgsParamsDescriptor>(mem);
        if (desc && desc->id == SCE_NGS_REVERB_PARAMS_STRUCT_ID)
            LOG_WARN_ONCE("Game is using unimplemented reverb audio module");
        return false;
    }
    const SceNgsReverbParams *const params = data.get_parameters<SceNgsReverbParams>(mem);
    float *const pcm = reinterpret_cast<float *>(data.parent->products[0].data);
    // without reverb parameters the buss passes its input on, as before
    if (!params || !pcm || params->desc.id != SCE_NGS_REVERB_PARAMS_STRUCT_ID)
        return false;

    const int32_t sample_rate = data.parent->rack->system->sample_rate;
    const int32_t granularity = data.parent->rack->system->granularity;
    if (sample_rate <= 0 || granularity <= 0)
        return false;
    ReverbRuntimeState *const state = data.get_runtime_state<ReverbRuntimeState>();
    if (state->sample_rate != sample_rate)
        state->init(sample_rate);

    const float rate = static_cast<float>(sample_rate);
    const float room = millibels(std::min(params->fRoom, 0.0f));
    const float dry = millibels(std::min(params->fDryMB, 0.0f));
    const float early_gain = room * millibels(std::min(params->fReflections, 1000.0f)) * std::max(params->fEarlyReflectionScalar, 0.0f);
    const float late_gain = room * millibels(std::min(params->fReverb, 2000.0f)) * WET_SCALE;
    const float decay = std::clamp(params->fDecayTime, 0.1f, 20.0f);
    const float decay_hf = decay * std::clamp(params->fDecayHFRatio, 0.1f, 2.0f);
    const float hf_reference = std::clamp(params->fHFReference, 20.0f, rate * 0.45f);
    const float cos_hf = std::cos(2.0f * PI * hf_reference / rate);
    // RoomHF: a lowpass on what enters the reverb
    const float input_lowpass = lowpass_coefficient(millibels(std::min(params->fRoomHF, 0.0f)), cos_hf);
    const float allpass_feedback = 0.3f + 0.3f * std::clamp(params->fDiffusion, 0.0f, 100.0f) / 100.0f;
    const size_t reflections_delay = static_cast<size_t>(std::clamp(params->fReflectionsDelay, 0.0f, 0.3f) * rate);
    const size_t late_delay = reflections_delay + static_cast<size_t>(std::clamp(params->fReverbDelay, 0.0f, 0.1f) * rate);
    const std::array<SceNgsReverbRoom, 2> patterns = { params->eEarlyReflectionPattern[0], params->eEarlyReflectionPattern[1] };

    // per comb: the loop gain for decay at low frequencies, and the damping that makes high ones
    // decay in decay_hf (RT60: -60 dB over the decay time)
    std::array<std::array<float, 8>, 2> feedback{}, damp{};
    for (int ch = 0; ch < 2; ch++) {
        for (size_t i = 0; i < 8; i++) {
            const float seconds = static_cast<float>(state->combs[ch][i].buffer.size()) / rate;
            const float g = std::pow(10.0f, -3.0f * seconds / decay);
            const float g_hf = std::pow(10.0f, -3.0f * seconds / decay_hf);
            feedback[ch][i] = std::min(g, 0.98f);
            damp[ch][i] = lowpass_coefficient(std::min(g_hf / g, 1.0f), cos_hf);
        }
    }

    for (int32_t n = 0; n < granularity; n++) {
        const float in_l = pcm[2 * n], in_r = pcm[2 * n + 1];
        // into the delay line, through RoomHF
        state->hf_state[0] = flush(in_l * (1.0f - input_lowpass) + state->hf_state[0] * input_lowpass);
        state->hf_state[1] = flush(in_r * (1.0f - input_lowpass) + state->hf_state[1] * input_lowpass);
        const size_t length = state->delay.size() / 2;
        state->delay_pos = (state->delay_pos + 1) % length;
        state->delay[state->delay_pos * 2] = state->hf_state[0];
        state->delay[state->delay_pos * 2 + 1] = state->hf_state[1];

        std::array<float, 2> out = { 0.0f, 0.0f };
        // early reflections
        for (int ch = 0; ch < 2; ch++) {
            const SceNgsReverbRoom pattern = patterns[ch];
            const auto &taps = (pattern % 2 == 0) ? EARLY_TAPS_LEFT : EARLY_TAPS_RIGHT;
            const float stretch = 1.0f + 0.5f * static_cast<float>(std::clamp<int>(pattern / 2, 0, 2));
            for (const auto &tap : taps)
                out[ch] += tap[1] * state->delayed(reflections_delay + static_cast<size_t>(tap[0] * stretch * rate / 1000.0f), ch);
            out[ch] *= early_gain;
        }
        // late reverb: mono into the combs, then the allpasses
        const float late_in = (state->delayed(late_delay, 0) + state->delayed(late_delay, 1)) * INPUT_GAIN;
        for (int ch = 0; ch < 2; ch++) {
            float late = 0.0f;
            for (size_t i = 0; i < 8; i++)
                late += state->combs[ch][i].process(late_in, feedback[ch][i], damp[ch][i]);
            for (Allpass &a : state->allpasses[ch])
                late = a.process(late, allpass_feedback);
            out[ch] += late * late_gain;
        }
        pcm[2 * n] = in_l * dry + out[0];
        pcm[2 * n + 1] = in_r * dry + out[1];
    }

    return false;
}
} // namespace ngs
