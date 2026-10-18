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

#include <ngs/modules/filter.h>
#include <util/log.h>

namespace ngs {

namespace {

struct FilterRuntimeState : public ModuleRuntimeState {
    BiquadState state;
};

} // namespace

std::unique_ptr<ModuleRuntimeState> FilterModule::create_runtime_state() const {
    return std::make_unique<FilterRuntimeState>();
}

void FilterModule::on_state_change(const MemState &mem, ModuleData &data, const VoiceState previous) {
    // a new play starts without the old samples in the filter
    if (data.parent->state == VOICE_STATE_ACTIVE && previous == VOICE_STATE_AVAILABLE)
        data.get_runtime_state<FilterRuntimeState>()->state.clear();
}

void FilterModule::set_default_preset(const MemState &mem, ModuleData &data) {
    if (!data.parent->rack->system->dsp)
        return; // other games keep what they had
    // off (the shape of Killzone Mercenary's own "off" setting; the hardware's default is a **hypothesis**)
    if (SceNgsFilterParams *const p = data.get_parameters<SceNgsFilterParams>(mem))
        *p = { { SCE_NGS_FILTER_PARAMS_STRUCT_ID, sizeof(SceNgsFilterParams) }, { SCE_NGS_FILTER_MODE_OFF, 1000.0f, 0.7f, 1.0f } };
}

bool FilterModule::process(KernelState &kern, const MemState &mem, const SceUID thread_id, ModuleData &data, std::unique_lock<std::recursive_mutex> &scheduler_lock, std::unique_lock<std::mutex> &voice_lock) {
    if (!data.is_bypassed) {
        const SceNgsParamsDescriptor *desc = data.get_parameters<SceNgsParamsDescriptor>(mem);
        if (desc && (desc->id == SCE_NGS_FILTER_PARAMS_COEFF_STRUCT_ID || desc->id == SCE_NGS_FILTER_PARAMS_STRUCT_ID)) {
            float *const pcm = reinterpret_cast<float *>(data.parent->products[0].data);
            const System *const system = data.parent->rack->system;
            if (!system->dsp) {
                LOG_WARN_ONCE("Game is using unimplemented filter audio module");
            } else if (pcm && system->granularity > 0) {
                const Biquad q = desc->id == SCE_NGS_FILTER_PARAMS_STRUCT_ID
                    ? biquad_from_params(data.get_parameters<SceNgsFilterParams>(mem)->params, static_cast<float>(system->sample_rate))
                    : biquad_from_coefficients(data.get_parameters<SceNgsFilterParamsCoEff>(mem)->params);
                biquad_process(q, data.get_runtime_state<FilterRuntimeState>()->state, pcm, system->granularity);
            }
        }
    }

    // Definitions with filters have 2 outputs
    data.parent->products[1] = data.parent->products[0];

    return false;
}
} // namespace ngs
