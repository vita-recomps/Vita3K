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

// The NGS filter modes as biquads (the filter and equalizer modules). The formulas are the RBJ
// audio-EQ cookbook's; how closely they match the hardware's is a **hypothesis**. fGain is a level
// in dB (Killzone Mercenary passes 20 log10 of its linear levels): the boost or cut of the peak and
// shelf modes, and an output level for the others.

#pragma once

#include <ngs/modules/filter.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace ngs {

struct Biquad {
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f; // normalised: y = b0 x + b1 x1 + b2 x2 - a1 y1 - a2 y2
    float out_gain = 1.0f;
    bool active = false;
};

struct BiquadState {
    std::array<std::array<float, 4>, 2> z{}; // per channel: x1, x2, y1, y2

    void clear() { z = {}; }
};

inline Biquad biquad_from_params(const SceNgsParamFilter &p, const float sample_rate) {
    Biquad q;
    if (p.eFilterMode == SCE_NGS_FILTER_MODE_OFF || sample_rate <= 0.0f)
        return q;
    constexpr float PI = 3.14159265358979f;
    const float f = std::clamp(p.fFrequency, 10.0f, sample_rate * 0.49f);
    const float w = 2.0f * PI * f / sample_rate;
    const float cw = std::cos(w), sw = std::sin(w);
    const float res = std::clamp(p.fResonance, 0.05f, 50.0f);
    const float alpha = sw / (2.0f * res);
    const float gain_db = std::clamp(p.fGain, -96.0f, 48.0f);
    const float A = std::pow(10.0f, gain_db / 40.0f);
    float b0, b1, b2, a0, a1, a2;
    bool level = true; // fGain is an output level, not part of the response
    switch (p.eFilterMode) {
    case SCE_NGS_FILTER_LOWPASS_RESONANT:
    case SCE_NGS_FILTER_LOWPASS_RESONANT_NORMALIZED:
        b0 = (1 - cw) / 2, b1 = 1 - cw, b2 = (1 - cw) / 2, a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha;
        break;
    case SCE_NGS_FILTER_HIGHPASS_RESONANT:
        b0 = (1 + cw) / 2, b1 = -(1 + cw), b2 = (1 + cw) / 2, a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha;
        break;
    case SCE_NGS_FILTER_BANDPASS_PEAK:
        b0 = res * alpha, b1 = 0, b2 = -res * alpha, a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha;
        break;
    case SCE_NGS_FILTER_BANDPASS_ZERO:
        b0 = alpha, b1 = 0, b2 = -alpha, a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha;
        break;
    case SCE_NGS_FILTER_NOTCH:
        b0 = 1, b1 = -2 * cw, b2 = 1, a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha;
        break;
    case SCE_NGS_FILTER_PEAK:
        b0 = 1 + alpha * A, b1 = -2 * cw, b2 = 1 - alpha * A, a0 = 1 + alpha / A, a1 = -2 * cw, a2 = 1 - alpha / A;
        level = false;
        break;
    case SCE_NGS_FILTER_HIGHSHELF: {
        const float s = 2 * std::sqrt(A) * alpha;
        b0 = A * ((A + 1) + (A - 1) * cw + s), b1 = -2 * A * ((A - 1) + (A + 1) * cw), b2 = A * ((A + 1) + (A - 1) * cw - s);
        a0 = (A + 1) - (A - 1) * cw + s, a1 = 2 * ((A - 1) - (A + 1) * cw), a2 = (A + 1) - (A - 1) * cw - s;
        level = false;
        break;
    }
    case SCE_NGS_FILTER_LOWSHELF: {
        const float s = 2 * std::sqrt(A) * alpha;
        b0 = A * ((A + 1) - (A - 1) * cw + s), b1 = 2 * A * ((A - 1) - (A + 1) * cw), b2 = A * ((A + 1) - (A - 1) * cw - s);
        a0 = (A + 1) + (A - 1) * cw + s, a1 = -2 * ((A - 1) + (A + 1) * cw), a2 = (A + 1) + (A - 1) * cw - s;
        level = false;
        break;
    }
    case SCE_NGS_FILTER_LOWPASS_ONEPOLE: {
        const float k = std::exp(-w);
        b0 = 1 - k, b1 = 0, b2 = 0, a0 = 1, a1 = -k, a2 = 0;
        break;
    }
    case SCE_NGS_FILTER_HIGHPASS_ONEPOLE: {
        const float k = std::exp(-w);
        b0 = (1 + k) / 2, b1 = -(1 + k) / 2, b2 = 0, a0 = 1, a1 = -k, a2 = 0;
        break;
    }
    case SCE_NGS_FILTER_ALLPASS:
        b0 = 1 - alpha, b1 = -2 * cw, b2 = 1 + alpha, a0 = 1 + alpha, a1 = -2 * cw, a2 = 1 - alpha;
        break;
    default:
        return q;
    }
    q.b0 = b0 / a0, q.b1 = b1 / a0, q.b2 = b2 / a0, q.a1 = a1 / a0, q.a2 = a2 / a0;
    q.out_gain = level ? A * A : 1.0f;
    q.active = true;
    return q;
}

inline Biquad biquad_from_coefficients(const SceNgsParamCoEff &c) {
    Biquad q;
    q.b0 = c.fB0, q.b1 = c.fB1, q.b2 = c.fB2, q.a1 = c.fA1, q.a2 = c.fA2;
    // all zero: a band left unset (it would silence the voice), as off
    q.active = c.fB0 != 0.0f || c.fB1 != 0.0f || c.fB2 != 0.0f || c.fA1 != 0.0f || c.fA2 != 0.0f;
    return q;
}

// filters `frames` stereo frames in place
inline void biquad_process(const Biquad &q, BiquadState &st, float *pcm, const int32_t frames) {
    if (!q.active)
        return;
    for (int ch = 0; ch < 2; ch++) {
        float x1 = st.z[ch][0], x2 = st.z[ch][1], y1 = st.z[ch][2], y2 = st.z[ch][3];
        for (int32_t n = 0; n < frames; n++) {
            const float x = pcm[2 * n + ch];
            float y = q.b0 * x + q.b1 * x1 + q.b2 * x2 - q.a1 * y1 - q.a2 * y2;
            if (!std::isfinite(y) || std::fabs(y) < 1e-25f)
                y = 0.0f;
            x2 = x1, x1 = x, y2 = y1, y1 = y;
            pcm[2 * n + ch] = y * q.out_gain;
        }
        st.z[ch] = { x1, x2, y1, y2 };
    }
}

} // namespace ngs
