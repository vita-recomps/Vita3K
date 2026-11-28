// Vita3K emulator project
// Contrast Adaptive Sharpening (CAS): a 5-tap version of AMD FidelityFX CAS's filter (its weights
// -1/lerp(8, 5, sharpness) and its amplitude from the local minimum and maximum). Leaves flat areas
// alone, sharpens edges, and clamps to the neighbourhood so edges don't ring.
// Compiled with: glslangValidator -V render_main_cas.frag -o render_main_cas.frag.spv
//
// Adapted from AMD FidelityFX CAS, under this licence:
//
// Copyright (c) 2020 Advanced Micro Devices, Inc. All rights reserved.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.
#version 450

layout(location = 0) in vec2 uv_frag;
layout(binding = 0) uniform sampler2D fb;

layout(location = 0) out vec3 color_frag;

layout ( push_constant ) uniform constants {
    vec2 inv_frame_size;
} pc;

// 0.0 = off, 1.0 = maximum
const float SHARPNESS = 0.75;

void main() {
    vec2 t = pc.inv_frame_size;

    vec3 b = texture(fb, uv_frag + vec2(-t.x, 0.0)).rgb;
    vec3 d = texture(fb, uv_frag + vec2(0.0, -t.y)).rgb;
    vec3 e = texture(fb, uv_frag).rgb;
    vec3 f = texture(fb, uv_frag + vec2( t.x, 0.0)).rgb;
    vec3 h = texture(fb, uv_frag + vec2(0.0,  t.y)).rgb;

    vec3 mn = min(min(min(d, f), min(b, h)), e);
    vec3 mx = max(max(max(d, f), max(b, h)), e);

    vec3 rcp = 1.0 / max(mx, vec3(1.0 / 255.0));
    vec3 amp = clamp(min(mn, vec3(2.0) - mx) * rcp, 0.0, 1.0);
    amp = sqrt(amp);

    // negative weights = sharpening; the denominator keeps the sum at 1
    vec3 w = -amp * (1.0 / (8.0 - 3.0 * SHARPNESS));
    vec3 sharpened = (b * w + d * w + f * w + h * w + e) / (1.0 + 4.0 * w);

    // stay inside the local range so edges don't ring
    color_frag = clamp(sharpened, mn, mx);
}
