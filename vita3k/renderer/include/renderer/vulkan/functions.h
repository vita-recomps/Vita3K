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

#include "renderer/vulkan/state.h"

struct Config;
struct MemState;

namespace renderer::vulkan {

// VITA3K_GPU_TRACE=1: note what each command list records; the last few lists submitted are written
// to gpu-trace.txt (in the working folder) when the device is lost, to find what the GPU hung on
bool gpu_trace_on();
void gpu_trace(VKContext &context, std::string entry);
void gpu_trace_submitted(VKContext &context, const char *what);
void gpu_trace_dump(const char *why);

bool create(std::unique_ptr<renderer::State> &state, const Config &config);

bool create(VKState &state, std::unique_ptr<Context> &context, MemState &mem);
bool create(VKState &state, std::unique_ptr<RenderTarget> &rt, const SceGxmRenderTargetParams &params, const FeatureState &features);
void destroy(VKState &state, std::unique_ptr<RenderTarget> &rt);
bool create(std::unique_ptr<VertexProgram> &vp, VKState &state, const SceGxmProgram &program);
bool create(std::unique_ptr<FragmentProgram> &fp, VKState &state, const SceGxmProgram &program, const SceGxmBlendInfo *blend);

void draw(VKContext &context, SceGxmPrimitiveType type, SceGxmIndexFormat format,
    Ptr<void> indices, size_t count, uint32_t instance_count, MemState &mem, const Config &config);

void mid_scene_flush(VKContext &context, const SceGxmNotification notification);
void new_frame(VKContext &context);
void signal_sync_object(VKState &state, SceGxmSyncObject *sync_object, uint32_t timestamp);

void set_context(VKContext &context, MemState &mem, VKRenderTarget *rt, const FeatureState &features);
void set_uniform_buffer(VKContext &context, MemState &mem, const ShaderProgram *program, const bool vertex_shader, const int block_num, const int size, Ptr<uint8_t> data);

void sync_clipping(VKContext &context);
void sync_stencil_func(VKContext &context, const bool is_back);
void sync_depth_bias(VKContext &context);
void sync_depth_data(VKContext &context);
void sync_stencil_data(VKContext &context, const MemState &mem);
void sync_point_line_width(VKContext &context, const bool is_front);
void sync_texture(VKContext &context, MemState &mem, std::size_t index, SceGxmTexture texture, const Config &config);
void sync_viewport_flat(VKContext &context);
void sync_viewport_real(VKContext &context, const float xOffset, const float yOffset, const float zOffset,
    const float xScale, const float yScale, const float zScale);
void sync_visibility_buffer(VKContext &context, Ptr<uint32_t> buffer, uint32_t stride);
void sync_visibility_index(VKContext &context, bool enable, uint32_t index, bool is_increment);

void refresh_pipeline(VKContext &context);

// Vulkan requires every vertex attribute address (offset + stride * index) to be a multiple of the
// component size of its format, and Metal only allows strides that are a multiple of 4. The PS Vita
// has neither restriction. A stream that breaks either rule is repacked into this layout, with every
// attribute at a 4-aligned offset; NVIDIA tolerates the original layout but AMD fetches garbage.
struct VertexStreamLayout {
    struct Attribute {
        uint32_t src_offset = 0;
        uint32_t dst_offset = 0;
        // number of bytes Vulkan fetches for this attribute
        uint32_t size = 0;
    };

    bool repack = false;
    // stride of the repacked stream (only meaningful if repack is true)
    uint32_t stride = 0;
    // indexed like SceGxmVertexProgram::attributes, only filled for the attributes of this stream
    std::vector<Attribute> attributes;
};

VertexStreamLayout get_vertex_stream_layout(const SceGxmVertexProgram &vertex_program, uint32_t stream_index);

} // namespace renderer::vulkan
