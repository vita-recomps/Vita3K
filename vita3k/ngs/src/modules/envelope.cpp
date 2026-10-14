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

#include <ngs/modules/envelope.h>
#include <util/log.h>

#include <algorithm>

namespace ngs {

namespace {

// the envelope's height `ms` milliseconds after key-on: linear between points (curved points are
// treated as linear), looping between uLoopStart and nLoopEnd when set, held at the last point
float envelope_height(const SceNgsEnvelopeParams &p, float ms, int32_t &point) {
    const uint32_t n = std::min<uint32_t>(p.uNumPoints, SCE_NGS_ENVELOPE_MAX_POINTS);
    if (n == 0) {
        point = 0;
        return 1.0f;
    }
    const bool loops = p.nLoopEnd >= 0 && static_cast<uint32_t>(p.nLoopEnd) < n && p.uLoopStart < static_cast<uint32_t>(p.nLoopEnd);
    float t = ms;
    uint32_t i = 0;
    for (int guard = 0; guard < 4096; guard++) {
        if (i + 1 >= n) {
            point = static_cast<int32_t>(n - 1);
            return p.envelopePoints[n - 1].fAmplitude;
        }
        const float span = static_cast<float>(p.envelopePoints[i].uMsecsToNextPoint);
        if (t < span) {
            point = static_cast<int32_t>(i);
            const float a = p.envelopePoints[i].fAmplitude, b = p.envelopePoints[i + 1].fAmplitude;
            return span > 0 ? a + (b - a) * (t / span) : b;
        }
        t -= span;
        i++;
        if (loops && i == static_cast<uint32_t>(p.nLoopEnd))
            i = p.uLoopStart;
    }
    point = static_cast<int32_t>(i);
    return p.envelopePoints[i].fAmplitude;
}

// the envelope's parameters, when they are set and the module is in use
const SceNgsEnvelopeParams *active_params(const MemState &mem, ModuleData &data) {
    if (data.is_bypassed || !data.info.data)
        return nullptr;
    const SceNgsEnvelopeParams *const params = data.get_parameters<SceNgsEnvelopeParams>(mem);
    if (!params || params->desc.id != SCE_NGS_ENVELOPE_PARAMS_STRUCT_ID)
        return nullptr;
    return params;
}

} // namespace

uint32_t voice_release_msecs(const MemState &mem, Voice &voice) {
    for (size_t i = 0; i < voice.rack->modules.size() && i < voice.datas.size(); i++) {
        if (!voice.rack->modules[i] || voice.rack->modules[i]->module_id() != 0x5CE3)
            continue;
        const SceNgsEnvelopeParams *const params = active_params(mem, voice.datas[i]);
        return params ? params->uReleaseMsecs : 0;
    }
    return 0;
}

void EnvelopeModule::on_state_change(const MemState &mem, ModuleData &data, const VoiceState previous) {
    if (data.guest_state_data.size() < sizeof(SceNgsEnvelopeStates))
        return;
    SceNgsEnvelopeStates *const state = data.get_state<SceNgsEnvelopeStates>();
    EnvelopeLogicalState *const logical = data.get_logical_state<EnvelopeLogicalState>();
    switch (data.parent->state) {
    case VOICE_STATE_ACTIVE:
        // key-on: the envelope starts again
        *state = {};
        state->fReleaseScale = 1.0f;
        *logical = {};
        break;
    case VOICE_STATE_FINALIZING: {
        // sceNgsVoiceKeyOff of a playing voice (the scheduler's own end of a voice sets is_keyed_off
        // first): with a release time, the voice goes on and process() fades it out
        const SceNgsEnvelopeParams *const params = active_params(mem, data);
        if (previous == VOICE_STATE_ACTIVE && !data.parent->is_keyed_off && params && params->uReleaseMsecs > 0) {
            logical->releasing = true;
            logical->release_ms = static_cast<float>(params->uReleaseMsecs);
            logical->elapsed_ms = 0.0f;
            logical->start_height = state->fCurrentHeight;
            state->nReleasing = 1;
            break;
        }
        // otherwise the voice ends at once, so the release has finished: games wait for
        // nReleasing == 1 and a zero height before they reuse a voice they keyed off
        logical->releasing = false;
        state->fCurrentHeight = 0.0f;
        state->nReleasing = 1;
        break;
    }
    case VOICE_STATE_AVAILABLE:
        logical->releasing = false;
        state->fCurrentHeight = 0.0f;
        state->nReleasing = 1;
        break;
    default:
        break;
    }
}

bool EnvelopeModule::process(KernelState &kern, const MemState &mem, const SceUID thread_id, ModuleData &data, std::unique_lock<std::recursive_mutex> &scheduler_lock, std::unique_lock<std::mutex> &voice_lock) {
    if (data.guest_state_data.size() < sizeof(SceNgsEnvelopeStates))
        return false;
    SceNgsEnvelopeStates *const state = data.get_state<SceNgsEnvelopeStates>();
    const int32_t granularity = data.parent->rack->system->granularity;
    const int32_t sample_rate = data.parent->rack->system->sample_rate;
    const float step_ms = sample_rate > 0 ? 1000.0f * static_cast<float>(granularity) / static_cast<float>(sample_rate) : 0.0f;

    float *const pcm = reinterpret_cast<float *>(data.parent->products[0].data);
    EnvelopeLogicalState *const logical = data.get_logical_state<EnvelopeLogicalState>();
    if (logical->releasing) {
        // the release: linear from the height at key-off to 0 over uReleaseMsecs (**hypothesis**:
        // the hardware's release shape is not known), then the voice ends with height 0
        const float r0 = logical->release_ms > 0 ? std::max(0.0f, 1.0f - logical->elapsed_ms / logical->release_ms) : 0.0f;
        const float r1 = logical->release_ms > 0 ? std::max(0.0f, 1.0f - (logical->elapsed_ms + step_ms) / logical->release_ms) : 0.0f;
        const float h0 = logical->start_height * r0, h1 = logical->start_height * r1;
        if (pcm && granularity > 0) {
            for (int32_t i = 0; i < granularity; i++) {
                const float g = h0 + (h1 - h0) * (static_cast<float>(i) / static_cast<float>(granularity));
                pcm[2 * i] *= g;
                pcm[2 * i + 1] *= g;
            }
        }
        logical->elapsed_ms += step_ms;
        state->fPosition += step_ms;
        state->fCurrentHeight = h1;
        state->nReleasing = 1;
        if (h1 > 0.0f)
            return false;
        // done: the scheduler ends the voice (finished callback, then available)
        logical->releasing = false;
        state->fCurrentHeight = 0.0f;
        return true;
    }

    if (data.is_bypassed) {
        state->fCurrentHeight = 1.0f;
        state->fPosition += step_ms;
        return false;
    }
    const SceNgsEnvelopeParams *const params = data.get_parameters<SceNgsEnvelopeParams>(mem);
    if (!params || params->desc.id != SCE_NGS_ENVELOPE_PARAMS_STRUCT_ID) {
        state->fCurrentHeight = 1.0f;
        state->fPosition += step_ms;
        return false;
    }

    // the gain moves from the height at the start of this block to the height at its end
    int32_t point = 0;
    const float h0 = envelope_height(*params, state->fPosition, point);
    const float h1 = envelope_height(*params, state->fPosition + step_ms, point);
    if (pcm && granularity > 0) {
        for (int32_t i = 0; i < granularity; i++) {
            const float g = h0 + (h1 - h0) * (static_cast<float>(i) / static_cast<float>(granularity));
            pcm[2 * i] *= g;
            pcm[2 * i + 1] *= g;
        }
    }
    state->fPosition += step_ms;
    state->fCurrentHeight = h1;
    state->nCurrentPoint = point;
    return false;
}
} // namespace ngs
