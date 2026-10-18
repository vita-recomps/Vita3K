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

// A feed-forward compressor. Killzone Mercenary's parameters read as: fRatio the slope above the
// threshold (0.1 for 10:1, 0.24 for about 4:1), fThreshold and fMakeupGain in dB, fAttack and
// fRelease the time constants in seconds, fSoftKnee a knee width in dB. That reading, and the
// detector's shape, are a **hypothesis**: the hardware's are not known.

#include <ngs/modules/compressor.h>
#include <util/log.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace ngs {

namespace {

struct CompressorRuntimeState : public ModuleRuntimeState {
    std::array<float, 2> envelope = { 0.0f, 0.0f }; // the detector's level per channel (linear, squared in RMS mode)
};

float to_db(const float linear) {
    return 20.0f * std::log10(std::max(linear, 1e-9f));
}

} // namespace

std::unique_ptr<ModuleRuntimeState> CompressorModule::create_runtime_state() const {
    return std::make_unique<CompressorRuntimeState>();
}

void CompressorModule::set_default_preset(const MemState &mem, ModuleData &data) {
    if (!data.parent->rack->system->dsp)
        return; // other games keep what they had
    // no compression (ratio 1), RMS, linked; the hardware's default is a **hypothesis**
    if (SceNgsCompressorParams *const p = data.get_parameters<SceNgsCompressorParams>(mem))
        *p = { { SCE_NGS_COMPRESSOR_PARAMS_STRUCT_ID, sizeof(SceNgsCompressorParams) }, 1.0f, 0.0f, 0.01f, 0.1f, 0.0f,
            SCE_NGS_COMPRESSOR_STEREO_LINK_ON, SCE_NGS_COMPRESSOR_RMS_MODE, 0.0f };
}

bool CompressorModule::process(KernelState &kern, const MemState &mem, const SceUID thread_id, ModuleData &data, std::unique_lock<std::recursive_mutex> &scheduler_lock, std::unique_lock<std::mutex> &voice_lock) {
    if (data.is_bypassed)
        return false;
    const SceNgsParamsDescriptor *desc = data.get_parameters<SceNgsParamsDescriptor>(mem);
    if (!desc || (desc->id != SCE_NGS_COMPRESSOR_PARAMS_STRUCT_ID && desc->id != SCE_NGS_COMPRESSOR_PARAMS_STRUCT_ID_V2))
        return false;
    const System *const system = data.parent->rack->system;
    if (!system->dsp) {
        LOG_WARN_ONCE("Game is using unimplemented compressor audio module");
        return false;
    }
    float *const pcm = reinterpret_cast<float *>(data.parent->products[0].data);
    if (!pcm || system->granularity <= 0 || system->sample_rate <= 0)
        return false;

    const SceNgsCompressorParams *const p = data.get_parameters<SceNgsCompressorParams>(mem);
    const float rate = static_cast<float>(system->sample_rate);
    const float slope = std::clamp(p->fRatio, 0.0f, 1.0f);
    const float threshold = std::clamp(p->fThreshold, -96.0f, 0.0f);
    const float knee = std::clamp(p->fSoftKnee, 0.0f, 48.0f);
    const float makeup = std::pow(10.0f, std::clamp(p->fMakeupGain, -48.0f, 48.0f) / 20.0f);
    const float attack = std::exp(-1.0f / (std::max(p->fAttack, 1e-4f) * rate));
    const float release = std::exp(-1.0f / (std::max(p->fRelease, 1e-4f) * rate));
    const bool rms = p->nPeakMode == SCE_NGS_COMPRESSOR_RMS_MODE;
    const bool link = p->nStereoLink == SCE_NGS_COMPRESSOR_STEREO_LINK_ON;

    CompressorRuntimeState *const state = data.get_runtime_state<CompressorRuntimeState>();
    // the static curve: gain in dB for an input level in dB
    const auto gain_db = [&](const float in_db) {
        const float over = in_db - threshold;
        if (knee > 0.0f && std::fabs(over) <= knee / 2.0f) {
            const float x = over + knee / 2.0f;
            return (slope - 1.0f) * x * x / (2.0f * knee);
        }
        return over > 0.0f ? (slope - 1.0f) * over : 0.0f;
    };
    for (int32_t n = 0; n < system->granularity; n++) {
        std::array<float, 2> level{};
        for (int ch = 0; ch < 2; ch++) {
            const float x = pcm[2 * n + ch];
            const float d = rms ? x * x : std::fabs(x);
            float &env = state->envelope[ch];
            env = d > env ? attack * env + (1.0f - attack) * d : release * env + (1.0f - release) * d;
            if (env < 1e-20f)
                env = 0.0f;
            level[ch] = rms ? std::sqrt(env) : env;
        }
        if (link)
            level[0] = level[1] = std::max(level[0], level[1]);
        for (int ch = 0; ch < 2; ch++)
            pcm[2 * n + ch] *= std::pow(10.0f, gain_db(to_db(level[ch])) / 20.0f) * makeup;
    }
    return false;
}
} // namespace ngs
