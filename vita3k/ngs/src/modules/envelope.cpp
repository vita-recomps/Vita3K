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

} // namespace

void EnvelopeModule::on_state_change(const MemState &mem, ModuleData &data, const VoiceState previous) {
    if (data.guest_state_data.size() < sizeof(SceNgsEnvelopeStates))
        return;
    SceNgsEnvelopeStates *const state = data.get_state<SceNgsEnvelopeStates>();
    switch (data.parent->state) {
    case VOICE_STATE_ACTIVE:
        // key-on: the envelope starts again
        *state = {};
        state->fReleaseScale = 1.0f;
        break;
    case VOICE_STATE_FINALIZING:
    case VOICE_STATE_AVAILABLE:
        // key-off or end of data: Vita3K ends the voice at once, so the release has finished
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
    float *const pcm = reinterpret_cast<float *>(data.parent->products[0].data);
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
