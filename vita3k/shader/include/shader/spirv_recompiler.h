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

#pragma once

#include <gxm/types.h>
#include <shader/usse_translator_types.h>
#include <shader/usse_types.h>

#include <features/state.h>

#include <string>
#include <vector>

namespace shader {

static constexpr int COLOR_ATTACHMENT_TEXTURE_SLOT_IMAGE = 0;
static constexpr int MASK_TEXTURE_SLOT_IMAGE = 1;
static constexpr int COLOR_ATTACHMENT_RAW_TEXTURE_SLOT_IMAGE = 3;
// 15 (ss-delta): depth clamped by the pipeline when supported, not in the vertex shader
// 16 (ss-delta): USSE min/max ignore a NaN operand
// 17 (vekta-mercenary): U2F10F10F10 colour output emulated behind a specialization constant
// 18 (vekta-mercenary): fragment shaders read the pixel's position from the global registers g23 and g24
// 19 (vekta-mercenary): with vpck_zero_fill, a pack keeps the lanes the previous pack into the same register wrote
// 20 (vekta-mercenary): shader interlock applies the fixed-function blend and colour mask; U2F10F10F10 for every fragment shader
// 21 (vekta-mercenary): a texture that is part of a surface is sampled within that part
// 22 (vekta-mercenary): buffers loaded into registers fill them even when no uniform parameter names them
static constexpr uint32_t CURRENT_VERSION = 22;
// fragment shader using the rendering surface as a storage image (because of shader interlock) have a line
// layout (constant_id = GAMMA_CORRECTION_SPECIALIZATIO_ID) const bool is_srgb = false;
// Setting this constant to true performs gamma correction in the shader
static constexpr uint32_t GAMMA_CORRECTION_SPECIALIZATION_ID = 0;
// every fragment shader (Vulkan) has a line
// layout (constant_id = U2F10F10F10_SPECIALIZATION_ID) const bool is_u2f10f10f10 = false;
// Setting it to true stores the colour as a U2F10F10F10 surface would (the surface itself is RGBA16F): RGB rounded to
// unsigned 10-bit floats, alpha clamped to [0, 1] and rounded to 2 bits
static constexpr uint32_t U2F10F10F10_SPECIALIZATION_ID = 1;
// fragment shaders using the surface as a storage image (shader interlock) do the fixed-function blend and colour mask
// themselves, from these constants (Vulkan's vk::BlendFactor, vk::BlendOp and vk::ColorComponentFlags values): a program
// that reads the colour may still be blended by the hardware (Killzone Mercenary multiplies its shadow map with one)
static constexpr uint32_t BLEND_ENABLE_SPECIALIZATION_ID = 2; // bool
static constexpr uint32_t BLEND_COLOR_SRC_SPECIALIZATION_ID = 3;
static constexpr uint32_t BLEND_COLOR_DST_SPECIALIZATION_ID = 4;
static constexpr uint32_t BLEND_COLOR_OP_SPECIALIZATION_ID = 5;
static constexpr uint32_t BLEND_ALPHA_SRC_SPECIALIZATION_ID = 6;
static constexpr uint32_t BLEND_ALPHA_DST_SPECIALIZATION_ID = 7;
static constexpr uint32_t BLEND_ALPHA_OP_SPECIALIZATION_ID = 8;
static constexpr uint32_t COLOR_MASK_SPECIALIZATION_ID = 9;

enum struct Target {
    GLSLOpenGL,
    SpirVOpenGL,
    SpirVVulkan
};

// Hints given while compiling the shader
// For example, the gxp shader knows at runtime the swizzle of the color surface
// or the format / number of components of the sampled images, but we can't
struct Hints {
    // used when symbols are stripped
    // also used when scaled attribute formats are not supported and conversion must be done in the shader
    const std::vector<SceGxmVertexAttribute> *attributes;
    // color format of the surface, used for the opengl renderer when the GPU does not support features.support_unknown_format
    // I'm asking for the format instead of the base format as the swizzle might be used in future updates for surfaces with only 1 or 2 components
    SceGxmColorFormat color_format;

    // we need to have the texture formats of the sampled textures in two cases:
    // - when doing dependent texture queries with unknown format we need to know the format
    // - when sampling, we need to know the component count of a texture
    SceGxmTextureFormat vertex_textures[SCE_GXM_MAX_TEXTURE_UNITS];
    SceGxmTextureFormat fragment_textures[SCE_GXM_MAX_TEXTURE_UNITS];
};

struct GeneratedShader {
    std::string glsl;
    usse::SpirvCode spirv;
};

// Dump generated SPIR-V disassembly up to this point
void spirv_disasm_print(const usse::SpirvCode &spirv_binary, std::string *spirv_dump = nullptr);

// the returned object will only have its glsl or spirv field non-empty depending on the target
GeneratedShader convert_gxp(const SceGxmProgram &program, const std::string &shader_hash, const FeatureState &features, const Target target, const Hints &hints, bool maskupdate = false,
    bool force_shader_debug = false, const std::function<bool(const std::string &ext, const std::string &dump)> &dumper = nullptr);

void convert_gxp_to_glsl_from_filepath(const std::string &shader_filepath_utf8);

} // namespace shader
