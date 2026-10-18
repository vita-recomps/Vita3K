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

#include "biquad.h"

#include <ngs/modules/equalizer.h>
#include <util/log.h>

namespace ngs {

namespace {

struct EqualizerRuntimeState : public ModuleRuntimeState {
    std::array<BiquadState, SCE_NGS_MAX_EQ_FILTERS> bands;
};

} // namespace

std::unique_ptr<ModuleRuntimeState> EqualizerModule::create_runtime_state() const {
    return std::make_unique<EqualizerRuntimeState>();
}

void EqualizerModule::on_state_change(const MemState &mem, ModuleData &data, const VoiceState previous) {
    if (data.parent->state == VOICE_STATE_ACTIVE && previous == VOICE_STATE_AVAILABLE)
        for (BiquadState &band : data.get_runtime_state<EqualizerRuntimeState>()->bands)
            band.clear();
}

void EqualizerModule::set_default_preset(const MemState &mem, ModuleData &data) {
    if (!data.parent->rack->system->dsp)
        return; // other games keep what they had
    // every band off (as the filter module's default)
    if (SceNgsParamEqParams *const p = data.get_parameters<SceNgsParamEqParams>(mem)) {
        p->desc = { SCE_NGS_PARAM_EQ_STRUCT_ID, sizeof(SceNgsParamEqParams) };
        for (SceNgsParamFilter &band : p->filter)
            band = { SCE_NGS_FILTER_MODE_OFF, 1000.0f, 0.7f, 1.0f };
    }
}

bool EqualizerModule::process(KernelState &kern, const MemState &mem, const SceUID thread_id, ModuleData &data, std::unique_lock<std::recursive_mutex> &scheduler_lock, std::unique_lock<std::mutex> &voice_lock) {
    if (!data.is_bypassed) {
        const SceNgsParamsDescriptor *desc = data.get_parameters<SceNgsParamsDescriptor>(mem);
        if (desc && (desc->id == SCE_NGS_PARAM_EQ_COEFF_STRUCT_ID || desc->id == SCE_NGS_PARAM_EQ_STRUCT_ID)) {
            float *const pcm = reinterpret_cast<float *>(data.parent->products[0].data);
            const System *const system = data.parent->rack->system;
            if (!system->dsp) {
                LOG_WARN_ONCE("Game is using unimplemented equalizer audio module");
            } else if (pcm && system->granularity > 0) {
                // the four bands in series
                EqualizerRuntimeState *const state = data.get_runtime_state<EqualizerRuntimeState>();
                for (int i = 0; i < SCE_NGS_MAX_EQ_FILTERS; i++) {
                    const Biquad q = desc->id == SCE_NGS_PARAM_EQ_STRUCT_ID
                        ? biquad_from_params(data.get_parameters<SceNgsParamEqParams>(mem)->filter[i], static_cast<float>(system->sample_rate))
                        : biquad_from_coefficients(data.get_parameters<SceNgsParamEqParamsCoEff>(mem)->filterCoEff[i]);
                    biquad_process(q, state->bands[i], pcm, system->granularity);
                }
            }
        }
    }

    // Definitions with equalizers can have up to 4 outputs
    data.parent->products[1] = data.parent->products[0];
    data.parent->products[2] = data.parent->products[0];
    data.parent->products[3] = data.parent->products[0];

    return false;
}
} // namespace ngs
