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

// A waveshaper: wet = clip(fA x + fB x^3) at +-fClip, silent under fGate, mixed with the dry
// input by fWetGain and fDryGain. Killzone Mercenary passes (A 2, clip 1, wet 1, dry 0) and
// (A 9, clip 1, wet 0.7, dry 0.7), and (wet 0, dry 1) to leave the sound as it is. The formula is a
// **hypothesis**: the hardware's curve is not known.

#include <ngs/modules/distortion.h>
#include <util/log.h>

#include <algorithm>
#include <cmath>

namespace ngs {

void DistortionModule::set_default_preset(const MemState &mem, ModuleData &data) {
    if (!data.parent->rack->system->dsp)
        return; // other games keep what they had
    // dry only (Killzone Mercenary's own "off" setting)
    if (SceNgsDistortionParams *const p = data.get_parameters<SceNgsDistortionParams>(mem))
        *p = { { SCE_NGS_DISTORTION_PARAMS_STRUCT_ID, sizeof(SceNgsDistortionParams) }, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f };
}

bool DistortionModule::process(KernelState &kern, const MemState &mem, const SceUID thread_id, ModuleData &data, std::unique_lock<std::recursive_mutex> &scheduler_lock, std::unique_lock<std::mutex> &voice_lock) {
    if (data.is_bypassed)
        return false;
    const SceNgsParamsDescriptor *desc = data.get_parameters<SceNgsParamsDescriptor>(mem);
    if (!desc || desc->id != SCE_NGS_DISTORTION_PARAMS_STRUCT_ID)
        return false;
    const System *const system = data.parent->rack->system;
    if (!system->dsp) {
        LOG_WARN_ONCE("Game is using unimplemented distortion audio module");
        return false;
    }
    float *const pcm = reinterpret_cast<float *>(data.parent->products[0].data);
    if (!pcm || system->granularity <= 0)
        return false;
    const SceNgsDistortionParams *const p = data.get_parameters<SceNgsDistortionParams>(mem);
    if (p->fWetGain == 0.0f && p->fDryGain == 1.0f)
        return false;
    const float clip = p->fClip > 0.0f ? p->fClip : 1.0f;
    for (int32_t i = 0; i < 2 * system->granularity; i++) {
        const float x = pcm[i];
        float wet = 0.0f;
        if (std::fabs(x) >= p->fGate)
            wet = std::clamp(p->fA * x + p->fB * x * x * x, -clip, clip);
        pcm[i] = wet * p->fWetGain + x * p->fDryGain;
    }
    return false;
}
} // namespace ngs
