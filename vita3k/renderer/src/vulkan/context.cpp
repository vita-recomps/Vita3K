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

#include <renderer/vulkan/types.h>

#include <renderer/vulkan/functions.h>
#include <renderer/vulkan/gxm_to_vulkan.h>
#include <renderer/vulkan/state.h>

#include <gxm/functions.h>
#include <renderer/functions.h>

#include <util/log.h>
#include <util/overloaded.h>

#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#ifdef _WIN32
// GetModuleHandleA (VITA3K_RENDERDOC_FRAME)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace renderer::vulkan {

static std::mutex gpu_trace_mutex;
static std::deque<std::vector<std::string>> gpu_trace_lists; // the last command lists submitted
static uint64_t gpu_trace_list_count = 0;

bool gpu_trace_on() {
    static const bool on = [] {
        const char *v = std::getenv("VITA3K_GPU_TRACE");
        return v && v[0] == '1';
    }();
    return on;
}

void gpu_trace(VKContext &context, std::string entry) {
    if (gpu_trace_on())
        context.gpu_trace_entries.push_back(std::move(entry));
}

void gpu_trace_submitted(VKContext &context, const char *what) {
    if (!gpu_trace_on())
        return;
    const std::lock_guard<std::mutex> lock(gpu_trace_mutex);
    context.gpu_trace_entries.push_back(fmt::format("submitted ({}), list #{}", what, ++gpu_trace_list_count));
    gpu_trace_lists.push_back(std::move(context.gpu_trace_entries));
    context.gpu_trace_entries.clear();
    while (gpu_trace_lists.size() > 6)
        gpu_trace_lists.pop_front();
}

void gpu_trace_dump(const char *why) {
    if (!gpu_trace_on())
        return;
    const std::lock_guard<std::mutex> lock(gpu_trace_mutex);
    std::ofstream out("gpu-trace.txt");
    out << "GPU trace (" << why << "): the last " << gpu_trace_lists.size() << " command lists submitted, oldest first\n";
    for (const auto &list : gpu_trace_lists) {
        out << "--- command list\n";
        for (const auto &entry : list)
            out << "  " << entry << "\n";
    }
    out.flush();
    LOG_ERROR("GPU trace written to gpu-trace.txt ({})", why);
}

// VITA3K_TRACE_ADDR=<guest address in hex>: when a surface sync copies a range holding it back to
// guest memory, log the 16 bytes there (copies 1500 to 1559; debugging aid)
static void trace_synced_bytes(const MemState &mem, Address location, uint32_t size) {
    static const Address traced = [] {
        const char *v = std::getenv("VITA3K_TRACE_ADDR");
        return v ? static_cast<Address>(std::strtoul(v, nullptr, 16)) : 0;
    }();
    if (!traced || traced < location || traced + 16 > location + size)
        return;
    static uint32_t count = 0;
    // VITA3K_TRACE_ADDR_FROM: the first copy logged (default 1500)
    static const uint32_t from = [] {
        const char *v = std::getenv("VITA3K_TRACE_ADDR_FROM");
        return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : 1500u;
    }();
    if (++count < from || count >= from + 60)
        return;
    const uint8_t *bytes = Ptr<const uint8_t>(traced).get(mem);
    std::string hex;
    for (int i = 0; i < 16; i++)
        hex += fmt::format("{:02x}{}", bytes[i], i % 4 == 3 ? " " : "");
    LOG_INFO("synced bytes at {} (copy {}): {}", log_hex(traced), count, hex);
    // and the whole range copied, every 10th of those, to a file in the working folder
    if (count % 10 == 0)
        std::ofstream(fmt::format("synced-{:08x}-{}.bin", location, count), std::ios::binary)
            .write(reinterpret_cast<const char *>(Ptr<const uint8_t>(location).get(mem)), size);
}

// VITA3K_QUERY_SCALE=<n>: occlusion query results written back to a visibility buffer are multiplied
// by n (debugging aid: the fork renders multisampled targets with one sample per pixel)
// VITA3K_TRACE_QUERIES=1: log what they hold (four in a row every 50 write-backs; debugging aid)
// VITA3K_SHADER_DEBUG=1 (with VITA3K_TRACE_VS): a patched box shader writes, for each object ID below 128, two
// corners (world position, clip position; corner 0 then corner 7) at its box table + 0x2000 + ID * 64. Log the
// non-zero ones from the GPU-side copy once the frame's work is done (debugging aid).
static void dump_shader_debug(VKState &state, uint64_t frame) {
    static const bool on = [] {
        const char *v = std::getenv("VITA3K_SHADER_DEBUG");
        return v && v[0] == '1';
    }();
    if (!on || frame % 30 != 0)
        return;
    const Address table = traced_block1_of_frame(frame);
    if (!table)
        return;
    const Address area = table + 0x2000;
    const auto it = state.mapped_memories.lower_bound(area);
    if (it == state.mapped_memories.end() || it->first + it->second.size < area + 128 * 64)
        return;
    const auto *buffer = std::get_if<vkutil::Buffer>(&it->second.buffer_impl);
    if (!buffer || !buffer->mapped_data)
        return;
    const float *values = reinterpret_cast<const float *>(static_cast<const uint8_t *>(buffer->mapped_data) + (area - it->first));
    for (int id = 0; id < 128; id++) {
        const float *v = values + id * 16;
        bool any = false;
        for (int i = 0; i < 16; i++)
            any |= v[i] != 0.0f;
        if (any)
            LOG_INFO("shader debug frame {} object {}: corner0 {} {} {} clip0 {} {} {} {} | corner7 {} {} {} clip7 {} {} {} {}", frame, id,
                v[0], v[1], v[2], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[12], v[13], v[14], v[15]);
    }
}

static void trace_query_results(const MemState &mem, Address location, uint32_t size, uint64_t frame) {
    static const uint32_t scale = [] {
        const char *v = std::getenv("VITA3K_QUERY_SCALE");
        return v ? static_cast<uint32_t>(std::strtoul(v, nullptr, 10)) : 0u;
    }();
    if (scale > 1) {
        uint32_t *results = Ptr<uint32_t>(location).get(mem);
        for (uint32_t i = 0; i < size / 4; i++)
            results[i] *= scale;
    }
    static const bool on = [] {
        const char *v = std::getenv("VITA3K_TRACE_QUERIES");
        return v && v[0] == '1';
    }();
    if (!on)
        return;
    static uint32_t count = 0;
    count++;
    // every frame of 1700-2099 (the opening cutscene of contract 02), and two of every 600
    if (!(frame >= 1700 && frame < 2100) && frame % 600 >= 2 && count % 50 >= 4)
        return;
    const uint32_t *words = Ptr<const uint32_t>(location).get(mem);
    const uint32_t n = size / 4;
    uint32_t zeros = 0;
    std::string sample;
    for (uint32_t i = 0; i < n; i++) {
        zeros += words[i] == 0;
        sample += fmt::format("{} ", words[i]);
    }
    LOG_INFO("query results at {} (write-back {}, frame {}): {} entries, {} zero: {}", log_hex(location), count, frame, n, zeros, sample);
}

void VKContext::wait_thread_function(const MemState &mem) {
    // try to wait for multiple fences at the same time if possible
    std::vector<vk::Fence> fences;

    auto wait_for_fences = [&]() {
        while (!fences.empty()) {
            // timeout so we can check for shutdown
            vk::Result result;
            try {
                result = state.device.waitForFences(fences, VK_TRUE, 100'000'000ULL);
            } catch (const vk::DeviceLostError &) {
                gpu_trace_dump("device lost while waiting for a fence");
                throw;
            }
            if (result == vk::Result::eSuccess) {
                // don't reset them
                fences.clear();
                return;
            }
            if (result == vk::Result::eTimeout) {
                if (state.request_queue.is_aborted()) {
                    fences.clear();
                    return;
                }
                continue;
            }
            LOG_ERROR("Could not wait for fences.");
            assert(false);
            fences.clear();
            return;
        }
    };

    while (true) {
        auto wait_request = state.request_queue.pop();

        if (!wait_request)
            break;

        std::visit(overloaded{
                       [&](FenceWaitRequest &request) {
                           fences.push_back(request.fence);
                       },
                       [&](NotificationRequest &request) {
                           if (request.notifications[0].address || request.notifications[1].address) {
                               wait_for_fences();

                               // same as in handle_sync_surface_data
                               std::unique_lock<std::mutex> lock(state.notification_mutex);

                               if (request.notifications[0].address)
                                   *request.notifications[0].address.get(mem) = request.notifications[0].value;
                               if (request.notifications[1].address)
                                   *request.notifications[1].address.get(mem) = request.notifications[1].value;

                               // unlocking before a notify should be faster
                               lock.unlock();
                               state.notification_ready.notify_all();
                           }
                       },
                       [&](FrameDoneRequest &request) {
                           wait_for_fences();

                           // don't reset them, the reset will be done in the new_frame function
                           // and these fences can still be waited for during texture uploading
                           std::unique_lock<std::mutex> lock(new_frame_mutex);
                           last_frame_waited = request.frame_timestamp;
                           lock.unlock();
                           new_frame_condv.notify_one();
                       },
                       [&](BufferSyncRequest &request) {
                           wait_for_fences();
                           if (request.source) {
                               memcpy(Ptr<void>(request.location).get(mem), request.source, request.size);
                               trace_synced_bytes(mem, request.location, request.size);
                               return;
                           }
                           auto mem_it = state.mapped_memories.lower_bound(request.location);
                           if (mem_it == state.mapped_memories.end() || mem_it->first + mem_it->second.size < request.location + request.size) {
                               LOG_ERROR("Buffer Sync request for {}-{} is not fully mapped", log_hex(request.location), log_hex(request.location + request.size));
                               return;
                           }
                           uint8_t *src = reinterpret_cast<uint8_t *>(std::get<vkutil::Buffer>(mem_it->second.buffer_impl).mapped_data);
                           src += request.location - mem_it->first;
                           memcpy(Ptr<void>(request.location).get(mem), src, request.size);
                           trace_synced_bytes(mem, request.location, request.size);
                           if (request.query_results) {
                               trace_query_results(mem, request.location, request.size, request.frame);
                               dump_shader_debug(state, request.frame);
                           }
                       },
                       [&](PostSurfaceSyncRequest &request) {
                           wait_for_fences();

                           state.surface_cache.perform_post_surface_sync(mem, request.cache_info);
                       },
                       [&](SyncSignalRequest &request) {
                           wait_for_fences();

                           renderer::subject_done(request.sync, request.timestamp);
                       },
                       [&](CallbackRequest &request) {
                           if (request.callback) {
                               (*request.callback)();
                               delete request.callback;
                           }
                       } },
            *wait_request);
    }
}

void set_context(VKContext &context, MemState &mem, VKRenderTarget *rt, const FeatureState &features) {
    context.render_target = rt;
    context.scene_timestamp++;
    context.state.texture_cache.current_scene_timestamp = context.scene_timestamp;

    SceGxmColorSurface *color_surface_fin = &context.record.color_surface;
    // set these values for the pipeline cache
    context.record.color_base_format = gxm::get_base_format(color_surface_fin->colorFormat);
    context.record.is_gamma_corrected = static_cast<bool>(color_surface_fin->gamma);
    vk::Format vk_format = color::translate_format(context.record.color_base_format);

    if (color_surface_fin->gamma && vk_format == vk::Format::eR8G8B8A8Unorm) {
        vk_format = vk::Format::eR8G8B8A8Srgb;
    }

    if (color_surface_fin->data.address() == 0) {
        color_surface_fin = nullptr;

        // set back default values
        vk_format = vk::Format::eR8G8B8A8Unorm;
        context.record.color_surface.downscale = static_cast<bool>(rt->multisample_mode);
        context.record.is_gamma_corrected = false;
        context.record.is_maskupdate = false;
        context.record.color_base_format = SCE_GXM_COLOR_BASE_FORMAT_U8U8U8U8;
    }
    context.current_color_format = vk_format;

    if (rt->multisample_mode && !context.record.color_surface.downscale) {
        // using MSAA without downscaling, emulate this as best as we can by multiplying the width and height of the render target by 2
        rt->width *= 2;
        rt->height *= 2;
    }

    SceGxmDepthStencilSurface *ds_surface_fin = &context.record.depth_stencil_surface;
    // if the depth-stencil buffer is not backed by memory or we don't read nor write it to memory, use the transient attachment instead
    // VITA3K_DEPTH_KEEP_UNFORCED=1: a depth-stencil surface backed by memory but with neither force load nor force store
    // is still kept as a surface (debugging aid: does a game read such a depth buffer as a texture?)
    static const bool keep_unforced = [] {
        const char *v = std::getenv("VITA3K_DEPTH_KEEP_UNFORCED");
        return v && v[0] == '1';
    }();
    if ((!ds_surface_fin->depth_data && !ds_surface_fin->stencil_data)
        || (!ds_surface_fin->force_load && !ds_surface_fin->force_store && !(keep_unforced && ds_surface_fin->depth_data))) {
        ds_surface_fin = nullptr;
    }

    VKState &state = context.state;
    state.surface_cache.set_render_target(rt);

    context.start_recording(true);

    bool force_load = context.record.depth_stencil_surface.force_load;
    bool force_store = context.record.depth_stencil_surface.force_store;
    if (ds_surface_fin == nullptr) {
        // no memory backing the depth stencil
        force_load = false;
        force_store = false;
    }
    if (context.state.features.support_shader_interlock)
        // we must always store the depth stencil
        force_store = true;
    context.current_render_pass = context.state.pipeline_cache.retrieve_render_pass(vk_format, force_load, force_store, color_surface_fin == nullptr);
    if (context.state.features.support_shader_interlock)
        // also retrieve / create the shader interlock pass
        context.current_shader_interlock_pass = context.state.pipeline_cache.retrieve_render_pass(vk_format, true, true, color_surface_fin == nullptr, true);

    Framebuffer &framebuffer = state.surface_cache.retrieve_framebuffer_handle(mem, color_surface_fin, ds_surface_fin, context.current_render_pass, context.current_shader_interlock_pass, context.current_color_view, context.current_ds_view);
    // a scene that loads its depth-stencil surface but doesn't store it leaves it as it was (FeatureState::depth_store_scenes)
    context.scene_depth_restore.reset();
    context.scene_stencil_store.reset();
    if (ds_surface_fin && ds_surface_fin->force_store)
        context.scene_stencil_store = *ds_surface_fin;
    if (context.state.features.depth_store_scenes && ds_surface_fin && ds_surface_fin->force_load && !ds_surface_fin->force_store
        && state.surface_cache.backup_scene_depth_stencil(context.prerender_cmd, *ds_surface_fin))
        context.scene_depth_restore = *ds_surface_fin;
    context.current_framebuffer = framebuffer.standard;
    context.current_shader_interlock_framebuffer = framebuffer.shader_interlock;
    context.current_color_base_image = framebuffer.base_image;
    context.current_color_storage_view = framebuffer.storage_view ? framebuffer.storage_view : context.current_color_view;

    // make sure we are not keeping any texture from the previous pass
    // (textures can be still bound even though they are not used)
    context.last_vert_texture_count = ~0;
    context.last_frag_texture_count = ~0;
    for (int i = 0; i < 16; i++) {
        context.vertex_textures[i].sampler = nullptr;
        context.fragment_textures[i].sampler = nullptr;
    }

    context.is_first_scene_draw = true;
    context.last_macroblock_x = ~0;
    context.last_macroblock_y = ~0;
    context.ignore_macroblock = false;
    context.drawn_since_macroblock_split = false;
}

void VKContext::restore_scene_depth() {
    if (!is_recording)
        return;
    if (scene_stencil_store) {
        if (in_renderpass)
            stop_render_pass();
        state.surface_cache.store_stencil_to_memory(render_cmd, *scene_stencil_store);
        scene_stencil_store.reset();
    }
    if (!scene_depth_restore)
        return;
    if (in_renderpass)
        stop_render_pass();
    state.surface_cache.restore_scene_depth_stencil(render_cmd, *scene_depth_restore);
    scene_depth_restore.reset();
}

void VKContext::start_recording(bool first_in_scene) {
    if (is_recording) {
        LOG_ERROR("Attempt to start recording while already recording");
        return;
    }

    if (render_target == nullptr) {
        LOG_ERROR("Recording started without a set command buffer");
        return;
    }

    if (render_target->last_used_frame != frame_timestamp) {
        // reset idx if we are in a new frame
        render_target->cmd_buffer_idx = 0;
        render_target->last_used_frame = frame_timestamp;
    }

    // safety check
    if (render_target->cmd_buffer_idx == render_target->cmd_buffers[state.current_frame_idx].size()) {
        LOG_WARN_ONCE("Render Target is using more scenes per frame than what was planned!");

        // add additional cmd buffers, fences and semaphores
        vk::CommandBufferAllocateInfo cmd_buffer_info{
            .commandPool = state.frame().render_pool,
            .commandBufferCount = 1
        };
        render_target->cmd_buffers[state.current_frame_idx].push_back(state.device.allocateCommandBuffers(cmd_buffer_info)[0]);

        cmd_buffer_info.commandPool = state.frame().prerender_pool;
        render_target->pre_cmd_buffers[state.current_frame_idx].push_back(state.device.allocateCommandBuffers(cmd_buffer_info)[0]);

        // we only use one fence per scene anyway
        vk::FenceCreateInfo fence_info{};
        // make sure the next fence used is the one we created (but only if this is the first recording of the scene)
        auto fence_insert_it = render_target->fences.begin() + render_target->fence_idx;
        if (!first_in_scene)
            fence_insert_it++;
        render_target->fences.insert(fence_insert_it, state.device.createFence(fence_info));
    }

    if (next_fence == nullptr) {
        next_fence = render_target->fences[render_target->fence_idx];
        // only increase the fence index if we used the previous one
        render_target->fence_idx = (render_target->fence_idx + 1) % render_target->fences.size();
    }

    render_cmd = render_target->cmd_buffers[state.current_frame_idx][render_target->cmd_buffer_idx];
    prerender_cmd = render_target->pre_cmd_buffers[state.current_frame_idx][render_target->cmd_buffer_idx];
    render_target->cmd_buffer_idx++;

    vk::CommandBufferBeginInfo begin_info{
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit
    };
    render_cmd.begin(begin_info);
    prerender_cmd.begin(begin_info);
    gpu_trace(*this, "begin command list");

    is_recording = true;

    // set all the dynamic state here
    render_cmd.setViewport(0, viewport);
    render_cmd.setScissor(0, scissor);
    sync_depth_bias(*this);
    sync_point_line_width(*this, true);
    sync_stencil_func(*this, false);
    if (record.two_sided == SCE_GXM_TWO_SIDED_ENABLED) {
        sync_stencil_func(*this, true);
    }
}

// we only need one descriptor per scene, so this does not need to be too big
static constexpr uint32_t DESCRIPTOR_PACK_SIZE = 16;

static vk::DescriptorSet retrieve_color_descriptor(VKState &state, FrameDescriptor &frame_descriptor) {
    if (frame_descriptor.descriptors_idx < frame_descriptor.sets.size())
        return frame_descriptor.sets[frame_descriptor.descriptors_idx++];

    // we have no more frame descriptor available, create a bunch of new one for this specific layout
    // the type depends on the way we read it
    vk::DescriptorPoolSize pool_size{
        .type = state.features.support_shader_interlock ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eInputAttachment,
        .descriptorCount = DESCRIPTOR_PACK_SIZE * MAX_FRAMES_RENDERING
    };

    vk::DescriptorPoolCreateInfo descriptor_pool_info{
        .maxSets = DESCRIPTOR_PACK_SIZE * MAX_FRAMES_RENDERING
    };
    descriptor_pool_info.setPoolSizes(pool_size);

    vk::DescriptorPool descriptor_pool = state.device.createDescriptorPool(descriptor_pool_info);
    state.frame_descriptor_pools.push_back(descriptor_pool);

    // allocate all the descriptor sets
    const vk::DescriptorSetLayout set_layout = state.pipeline_cache.attachments_layout;
    std::vector<vk::DescriptorSetLayout> layouts(DESCRIPTOR_PACK_SIZE * MAX_FRAMES_RENDERING, set_layout);
    vk::DescriptorSetAllocateInfo descr_set_info{
        .descriptorPool = descriptor_pool
    };
    descr_set_info.setSetLayouts(layouts);
    auto descriptor_sets = state.device.allocateDescriptorSets(descr_set_info);

    // distribute them among all frames
    for (int frame_idx = 0; frame_idx < MAX_FRAMES_RENDERING; frame_idx++) {
        FrameDescriptor &frame_descr = state.frames[frame_idx].color_descriptor;

        // insert DESCRIPTOR_PACK_SIZE in each frame descriptor
        auto descr_it = descriptor_sets.begin() + frame_idx * DESCRIPTOR_PACK_SIZE;
        frame_descr.sets.insert(frame_descr.sets.end(), descr_it, descr_it + DESCRIPTOR_PACK_SIZE);
    }

    return frame_descriptor.sets[frame_descriptor.descriptors_idx++];
}

void VKContext::start_render_pass(bool create_descriptor_set) {
    if (in_renderpass) {
        LOG_ERROR("Starting render pass while already in render pass");
        return;
    }

    if (!is_recording)
        start_recording();

    curr_renderpass_info = vk::RenderPassBeginInfo{
        .renderPass = current_render_pass,
        .framebuffer = current_framebuffer
    };

    if (render_target->has_macroblock_sync && !ignore_macroblock) {
        // set the render area to the correct macroblock
        curr_renderpass_info.renderArea = vk::Rect2D{
            .offset = {
                last_macroblock_x * render_target->macroblock_width,
                last_macroblock_y * render_target->macroblock_height },
            .extent = { render_target->macroblock_width, render_target->macroblock_height }
        };
    } else {
        curr_renderpass_info.renderArea = vk::Rect2D{
            .offset = { 0, 0 },
            .extent = { render_target->width, render_target->height }
        };
    }

    // only the depth-stencil attachment may be clear if not force loaded
    std::array<vk::ClearValue, 2> curr_clear_values{};
    curr_clear_values[1].depthStencil = vk::ClearDepthStencilValue{
        .depth = record.depth_stencil_surface.background_depth,
        .stencil = record.depth_stencil_surface.stencil
    };
    curr_renderpass_info.setClearValues(curr_clear_values);
    render_cmd.beginRenderPass(curr_renderpass_info, vk::SubpassContents::eInline);
    gpu_trace(*this, fmt::format("begin render pass {}x{}", curr_renderpass_info.renderArea.extent.width, curr_renderpass_info.renderArea.extent.height));

    // set the renderpass info ready in case we need to switch between classic and framebuffer fetch usage
    curr_renderpass_info.setClearValues(nullptr);
    last_draw_was_framebuffer_fetch = false;

    refresh_pipeline = true;
    current_pipeline = nullptr;
    in_renderpass = true;

    if (!create_descriptor_set)
        return;

    // update the rendertarget descriptor set
    rendertarget_set = retrieve_color_descriptor(state, state.frame().color_descriptor);

    // update descriptor set for the whole scene with the color attachment
    vk::DescriptorImageInfo descr_color_info{
        .sampler = nullptr,
        .imageView = state.features.support_shader_interlock ? current_color_storage_view : current_color_view,
        .imageLayout = vk::ImageLayout::eGeneral,
    };

    const vk::DescriptorType input_type = state.features.support_shader_interlock ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eInputAttachment;
    vk::WriteDescriptorSet write_descr{
        .dstSet = rendertarget_set,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorType = input_type,
    };
    write_descr.setImageInfo(descr_color_info);
    state.device.updateDescriptorSets(write_descr, {});
}

void VKContext::stop_render_pass() {
    if (!in_renderpass) {
        LOG_ERROR("Stopping render pass while not in render pass");
        return;
    }

    // do this before ending the render pass
    if (is_in_query) {
        render_cmd.endQuery(current_visibility_buffer->query_pool, current_query_idx);
        gpu_trace(*this, fmt::format("end query {}", current_query_idx));
        is_in_query = false;
    }

    render_cmd.endRenderPass();
    gpu_trace(*this, "end render pass");

    in_renderpass = false;
}

void VKContext::stop_recording(const SceGxmNotification &notif1, const SceGxmNotification &notif2, bool submit) {
    if (!is_recording) {
        LOG_ERROR("Stopping recording while not recording");
        return;
    }

    if (in_renderpass)
        stop_render_pass();

    struct VisibilityRange {
        uint32_t offset;
        uint32_t size;
    };
    // the guest memory each read-back range of query results goes to (synced after the submit)
    struct VisibilitySync {
        Address address;
        uint32_t size;
    };
    std::vector<VisibilitySync> visibility_syncs;
    if (visibility_max_used_idx != -1) {
        // Queries may have been begun in several visibility buffers during this command list (a
        // game can switch buffers between scenes). Each buffer's queries are reset and read back
        // from its own record: reading back queries that were never begun, with the wait flag, makes
        // the GPU wait forever (Killzone Mercenary's later levels hung the GPU this way).
        if (visibility_buffers_used.size() > 1)
            LOG_WARN_ONCE("Occlusion queries in {} visibility buffers in one command list", visibility_buffers_used.size());
        for (VisibilityBuffer *buffer : visibility_buffers_used) {
            // get all the entry ranges that were used (queries_used has one more entry than queries)
            std::vector<VisibilityRange> occlusion_ranges;
            bool in_range = false;
            uint32_t range_start = 0;
            for (uint32_t entry = 0; entry <= static_cast<uint32_t>(buffer->max_used_idx) + 1; entry++) {
                if (buffer->queries_used[entry] == in_range)
                    continue;

                if (in_range) {
                    occlusion_ranges.push_back({ range_start, entry - range_start });
                    in_range = false;
                } else {
                    range_start = entry;
                    in_range = true;
                }
            }

            for (auto &range : occlusion_ranges) {
                // reset before the beginning of the render pass
                prerender_cmd.resetQueryPool(buffer->query_pool, range.offset, range.size);
                gpu_trace(*this, fmt::format("reset queries {}+{} (pre-render list) and read them back with wait", range.offset, range.size));

                // wait for the range at the end
                // TODO: this will be wrong with upscaling enabled and precise mode set
                render_cmd.copyQueryPoolResults(buffer->query_pool, range.offset, range.size,
                    buffer->gpu_buffer, buffer->buffer_offset + range.offset * sizeof(uint32_t),
                    sizeof(uint32_t), vk::QueryResultFlagBits::eWait);
                // VITA3K_ALL_VISIBLE=1: every query reports a large sample count, as if everything were
                // visible (debugging aid: does occlusion culling make objects flicker?)
                static const bool all_visible = [] {
                    const char *v = std::getenv("VITA3K_ALL_VISIBLE");
                    return v && v[0] == '1';
                }();
                if (all_visible) {
                    const vk::BufferMemoryBarrier after_copy{
                        .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                        .dstAccessMask = vk::AccessFlagBits::eTransferWrite,
                        .buffer = buffer->gpu_buffer,
                        .offset = buffer->buffer_offset + range.offset * sizeof(uint32_t),
                        .size = range.size * sizeof(uint32_t)
                    };
                    render_cmd.pipelineBarrier(vk::PipelineStageFlagBits::eTransfer, vk::PipelineStageFlagBits::eTransfer, {}, {}, after_copy, {});
                    render_cmd.fillBuffer(buffer->gpu_buffer, buffer->buffer_offset + range.offset * sizeof(uint32_t), range.size * sizeof(uint32_t), 0x00FFFFFFu);
                }
                visibility_syncs.push_back({ buffer->address + range.offset * 4, range.size * 4 });
            }
            buffer->max_used_idx = -1;
            buffer->queries_used.assign(buffer->size + 1, false);
        }
        visibility_buffers_used.clear();
        visibility_max_used_idx = -1;
    }

    ColorSurfaceCacheInfo *surface_info = nullptr;
    if (state.features.enable_memory_mapping && !state.disable_surface_sync && submit)
        surface_info = state.surface_cache.perform_surface_sync();

    prerender_cmd.end();
    render_cmd.end();

    // the prerender cmd must be submitted before the render cmd, the pipeline barriers do the rest
    cmdbuffers_to_submit.push_back(prerender_cmd);
    cmdbuffers_to_submit.push_back(render_cmd);

    render_cmd = nullptr;
    prerender_cmd = nullptr;
    is_recording = false;

    if (!submit)
        return;

    if (render_target->multisample_mode && !record.color_surface.downscale) {
        // revert changes made in set_context
        render_target->width /= 2;
        render_target->height /= 2;
    }

    vk::Fence fence = next_fence;
    next_fence = nullptr;

    vk::SubmitInfo submit_info{};
    submit_info.setCommandBuffers(cmdbuffers_to_submit);

    gpu_trace_submitted(*this, "render");
    state.general_queue.submit(submit_info, fence);
    cmdbuffers_to_submit.clear();
    state.frame().rendered_fences.push_back(fence);

    if (state.features.enable_memory_mapping) {
        // send it to the wait queue
        state.request_queue.push(FenceWaitRequest{ fence });

        if (state.mapping_method == MappingMethod::DoubleBuffer) {
            // sync all the visibility buffers
            for (const VisibilitySync &sync : visibility_syncs) {
                state.request_queue.push(BufferSyncRequest{ .location = sync.address, .size = sync.size, .query_results = true, .frame = frame_timestamp });
            }

            // we must sync the two buffers
            if (surface_info && surface_info->need_buffer_sync) {
                const vkutil::Buffer *readback = surface_info->readback_buffer.get();
                const uint32_t size = static_cast<uint32_t>(surface_info->total_bytes);
                if (readback && readback->mapped_data && readback->size >= size)
                    state.request_queue.push(BufferSyncRequest{ surface_info->data.address(), size, static_cast<const uint8_t *>(readback->mapped_data) });
                else
                    state.request_queue.push(BufferSyncRequest{ surface_info->data.address(), size });
            }
        }

        if (surface_info && surface_info->need_post_surface_sync) {
            state.request_queue.push(PostSurfaceSyncRequest{ surface_info });
        }

        if (notif1.address || notif2.address) {
            // notifications last
            NotificationRequest request = {
                .notifications = { notif1, notif2 },
            };
            state.request_queue.push(request);
        }
    }
}

void VKContext::check_for_macroblock_change(bool is_draw) {
    if (!render_target->has_macroblock_sync)
        return;

    if (!ignore_macroblock && (scissor.extent.width > render_target->macroblock_width || scissor.extent.height > render_target->macroblock_height)) {
        // flower does not specify a scissor adapted to the current macroblock
        // so fallback to the slow path (one scene per draw, can't really do better)
        // TODO: with the feedback loop extension we can do better
        ignore_macroblock = true;
        // in this case we must load and store the depth stencil each time
        current_render_pass = state.pipeline_cache.retrieve_render_pass(current_color_format, true, true, !record.color_surface.data);
    }

    // use the scissor to know in which macroblock we are
    uint16_t curr_macroblock_x = scissor.offset.x / render_target->macroblock_width;
    uint16_t curr_macroblock_y = scissor.offset.y / render_target->macroblock_height;

    // without a usable macroblock, every draw gets its own render pass (and, without texture
    // viewports, its own recording). The split must come before the draw's textures are synced: a
    // texture read from this surface is copied in the recording's prerender commands, which run
    // before the draws already recorded. Splitting at the draw itself left that copy in the previous
    // draw's recording, so each draw sampled what the previous one wrote a frame earlier (Soul
    // Sacrifice Delta's blur chains: its bloom trailed moving objects)
    const bool split_after_draw = ignore_macroblock && drawn_since_macroblock_split;
    if (is_draw)
        drawn_since_macroblock_split = true;
    if (split_after_draw || curr_macroblock_x != last_macroblock_x || curr_macroblock_y != last_macroblock_y) {
        if (split_after_draw)
            drawn_since_macroblock_split = is_draw;
        // we changed the current macroblock, restart the renderpass
        last_macroblock_x = curr_macroblock_x;
        last_macroblock_y = curr_macroblock_y;

        if (in_renderpass) {
            if (state.features.use_texture_viewport) {
                // no need to copy the texture after this renderpass
                stop_render_pass();
            } else {
                SceGxmNotification empty_notification{};
                stop_recording(empty_notification, empty_notification, false);
                start_recording();
                scene_timestamp++;
            }
        }
    }
}

// VITA3K_RENDERDOC_FRAME=<n>[,<count>]: with RenderDoc's Vulkan layer loaded (ENABLE_VULKAN_RENDERDOC_CAPTURE=1), capture
// the game's frames n to n+count-1 (one capture; default count 1) into renderdoc/ in the working folder (debugging aid).
// The minimal part of renderdoc_app.h (MIT, Baldur Karlsson) this needs:
namespace renderdoc_api {
typedef void(__cdecl *StartEndFrameCapture)(void *device, void *window);
typedef void(__cdecl *SetCaptureFilePathTemplate)(const char *path_template);
typedef int(__cdecl *GetAPI)(int version, void **api);
typedef void(__cdecl *MaskOverlayBits)(uint32_t and_mask, uint32_t or_mask);
// RENDERDOC_API_1_1_2's table: the functions this uses are at these indices
constexpr int mask_overlay_bits = 8;
constexpr int set_capture_file_path_template = 11;
constexpr int start_frame_capture = 19;
constexpr int end_frame_capture = 21;
} // namespace renderdoc_api

static void renderdoc_frame(uint64_t frame) {
    static void **api = nullptr;
    static uint64_t first = 0, count = 1;
    // VITA3K_RENDERDOC_FRAME=file: capture the next frame each time the file renderdoc-capture.on appears in the
    // working folder (it is removed when the capture ends)
    static bool on_file = false;
    static bool file_capturing = false;
    static const bool wanted = [] {
        const char *v = std::getenv("VITA3K_RENDERDOC_FRAME");
        if (!v)
            return false;
        on_file = std::string(v) == "file";
        first = std::strtoull(v, nullptr, 10);
        if (const char *comma = std::strchr(v, ','))
            count = std::max<uint64_t>(1, std::strtoull(comma + 1, nullptr, 10));
#ifdef _WIN32
        HMODULE module = GetModuleHandleA("renderdoc.dll");
        if (!module) {
            LOG_WARN("VITA3K_RENDERDOC_FRAME: RenderDoc is not loaded (set ENABLE_VULKAN_RENDERDOC_CAPTURE=1)");
            return false;
        }
        auto get_api = reinterpret_cast<renderdoc_api::GetAPI>(GetProcAddress(module, "RENDERDOC_GetAPI"));
        // eRENDERDOC_API_Version_1_1_2
        if (!get_api || !get_api(10102, reinterpret_cast<void **>(&api)) || !api) {
            LOG_WARN("VITA3K_RENDERDOC_FRAME: no RenderDoc API");
            return false;
        }
        reinterpret_cast<renderdoc_api::SetCaptureFilePathTemplate>(api[renderdoc_api::set_capture_file_path_template])("renderdoc/vekta");
        // no overlay: it covers the top left of the picture, where screen checks look
        reinterpret_cast<renderdoc_api::MaskOverlayBits>(api[renderdoc_api::mask_overlay_bits])(0, 0);
        return true;
#else
        return false;
#endif
    }();
    if (!wanted)
        return;
    if (on_file) {
        if (file_capturing) {
            reinterpret_cast<renderdoc_api::StartEndFrameCapture>(api[renderdoc_api::end_frame_capture])(nullptr, nullptr);
            std::error_code error;
            std::filesystem::remove("renderdoc-capture.on", error);
            file_capturing = false;
            LOG_INFO("RenderDoc: capture of frame {} ended", frame - 1);
        } else if (std::filesystem::exists("renderdoc-capture.on")) {
            reinterpret_cast<renderdoc_api::StartEndFrameCapture>(api[renderdoc_api::start_frame_capture])(nullptr, nullptr);
            file_capturing = true;
            LOG_INFO("RenderDoc: capturing frame {}", frame);
        }
        return;
    }
    if (frame == first) {
        reinterpret_cast<renderdoc_api::StartEndFrameCapture>(api[renderdoc_api::start_frame_capture])(nullptr, nullptr);
        LOG_INFO("RenderDoc: capturing frames {} to {}", first, first + count - 1);
    } else if (frame == first + count) {
        reinterpret_cast<renderdoc_api::StartEndFrameCapture>(api[renderdoc_api::end_frame_capture])(nullptr, nullptr);
        LOG_INFO("RenderDoc: capture ended");
    }
}

void new_frame(VKContext &context) {
    renderdoc_frame(context.frame_timestamp);
    if (context.state.features.enable_memory_mapping) {
        FrameDoneRequest request = { context.frame_timestamp };
        context.state.request_queue.push(request);

        context.state.surface_cache.clear_surfaces_changed();
    }

    context.frame_timestamp++;
    context.state.current_frame_idx = context.frame_timestamp % MAX_FRAMES_RENDERING;

    vk::Device device = context.state.device;
    FrameObject &frame = context.state.frame();

    // wait on all fences still present to make sure
    if (!frame.rendered_fences.empty()) {
        // wait for the fences, then reset them

        if (context.state.features.enable_memory_mapping) {
            // this will underflow for the first MAX_FRAMES_RENDERING frames
            // but that's not an issue as frame.rendered_fences will be empty
            uint64_t previous_frame_timestamp = context.frame_timestamp - MAX_FRAMES_RENDERING;

            // the wait is done by the wait thread
            std::unique_lock<std::mutex> lock(context.new_frame_mutex);
            context.new_frame_condv.wait(lock, [&]() {
                return context.last_frame_waited >= previous_frame_timestamp;
            });
        } else {
            auto result = device.waitForFences(frame.rendered_fences, VK_TRUE, std::numeric_limits<uint64_t>::max());
            if (result != vk::Result::eSuccess) {
                LOG_ERROR("Could not wait for fences.");
                assert(false);
                return;
            }
        }

        // reset the fences in both case (the wait thread does not do that as they can still be used)
        device.resetFences(frame.rendered_fences);
        frame.rendered_fences.clear();
    }

    device.resetCommandPool(frame.prerender_pool);
    device.resetCommandPool(frame.render_pool);

    // set the position in the used descriptor queue back to the beginning
    for (int i = 0; i < 16; i++) {
        frame.vert_descriptors[i].descriptors_idx = 0;
        frame.frag_descriptors[i].descriptors_idx = 0;
    }
    frame.color_descriptor.descriptors_idx = 0;

    // deferred destruction of the objects
    frame.destroy_queue.destroy_objects();

    context.last_vert_texture_count = ~0;
    context.last_frag_texture_count = ~0;

    frame.frame_timestamp = context.frame_timestamp;
}

void signal_sync_object(VKState &state, SceGxmSyncObject *sync_object, uint32_t timestamp) {
    assert(state.features.enable_memory_mapping);

    SyncSignalRequest request{
        .sync = sync_object,
        .timestamp = timestamp
    };
    state.request_queue.push(request);
}

} // namespace renderer::vulkan
