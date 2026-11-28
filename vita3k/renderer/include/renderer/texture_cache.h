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
#include <util/containers.h>
#include <util/fs.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

namespace ddspp {
struct Descriptor;
}

struct MemState;

enum SceGxmTextureBaseFormat : uint32_t;

namespace renderer {
struct YUVConversionCache {
    void *sws_context = nullptr;
    size_t width = 0;
    size_t height = 0;
    bool is_p3 = false;
};

enum class Backend : uint32_t;
static constexpr size_t TextureCacheSize = 1024;

typedef std::array<uint32_t, 4> TextureGxmDataRepr;
struct TextureCacheInfo {
    uint64_t hash = 0;
    SceGxmTexture texture;
    int index = 0;
    uint32_t texture_size = 0;
    bool use_hash = false;
    // no need for it to be atomic
    bool dirty = false;
    // used for texture importation
    bool is_imported = false;
    bool is_srgb = false;
    uint16_t width = 0;
    uint16_t height = 0;
    uint16_t mip_count = 0;
    SceGxmTextureBaseFormat format;
    // its mips are generated (State::generate_mips): uploaded decompressed, level 0 only
    bool generate_mips = false;
};

struct SamplerCacheInfo {
    // compact representation of the sampler state
    uint32_t value = 0;
    int index = 0;
};

struct AvailableTexture {
    bool is_dds;
    std::shared_ptr<fs::path> folder_path;
    // made in memory by the TextureOverride (RGBA8 in override_pixels), not read from a file
    bool in_memory = false;
};

// A game's own pictures for some of its textures, made while the game runs (the single-game front
// end's button labels, game-frontend/button_labels.h). The cache asks it about each texture it
// uploads whose format and size `wants` accepts; `make` gets the texture's first level as the game
// holds it and returns RGBA8 pixels of any size (uploaded as an imported texture), or false to keep
// the game's own. Called on the render thread.
class TextureOverride {
public:
    virtual ~TextureOverride() = default;
    virtual bool wants(SceGxmTextureBaseFormat format, uint32_t width, uint32_t height) const = 0;
    virtual bool make(SceGxmTextureBaseFormat format, uint32_t width, uint32_t height, const uint8_t *data, uint32_t size,
        std::vector<uint8_t> &rgba, uint32_t &out_width, uint32_t &out_height)
        = 0;
};

class TextureCache {
protected:
    // current texture info the cache is looking at
    TextureCacheInfo *current_info = nullptr;

    // are we in the process of exporting a texture
    bool exporting_texture = false;
    // are we in the process of importing a texture
    bool importing_texture = false;

    // dds/png raw file
    std::vector<uint8_t> imported_texture_raw_data;
    // the TextureOverride's picture for the texture being uploaded (AvailableTexture::in_memory)
    std::vector<uint8_t> override_pixels;
    uint32_t override_width = 0, override_height = 0;
    std::atomic<bool> texture_override_changed{ false };
    // marks every cached texture for upload at its next use
    void invalidate_all_textures();
    // pointer to the decoded content
    uint8_t *imported_texture_decoded = nullptr;
    // Info about the texture currently loading
    AvailableTexture loading_texture;
    // contain the decrypted header when loading dds
    ddspp::Descriptor *dds_descriptor = nullptr;
    // file being written to when exporting dds
    fs::ofstream output_file;
    // used when exporting dds
    bool export_dds_swap_rb = false;

    bool import_textures = false;
    // if set to false, save textures as dds
    bool save_as_png = true;
    bool export_textures = false;

public:
    Backend backend;
    bool use_protect = false;
    YUVConversionCache yuv_conversion_cache;
    // use a separate sampler cache
    bool use_sampler_cache = false;
    int anisotropic_filtering = 1;

    // used to quickly get the info from a hash of a gxm_texture
    unordered_map_fast<TextureGxmDataRepr, TextureCacheInfo *> texture_lookup;
    lru::Queue<TextureCacheInfo> texture_queue;

    // when use_sampler_cache is set to true, used to quickly get a cached sampler
    unordered_map_fast<uint32_t, SamplerCacheInfo *> sampler_lookup;
    lru::Queue<SamplerCacheInfo> sampler_queue;
    size_t last_bound_sampler_index;

    // folder where the replacement textures should be located
    fs::path import_folder;
    // key = hash, content = is the texture a dds (true) or a png (false)
    unordered_map_fast<uint64_t, AvailableTexture> available_textures_hash;

    // folder where the exported textures will be saved
    fs::path export_folder;
    // hash of the textures that have already been exported
    unordered_set_fast<uint64_t> exported_textures_hash;

    // smartphone GPUs do not support DXT (BC1/2/3/4/5) textures, they must be decompressed on the GPU
    bool support_dxt = false;
    // format for replaced texture, supported mostly by smartphone GPUs
    bool support_astc = false;
    // some smartphone GPUs do not support linear filtering on depth surfaces
    bool support_depth_linear_filtering = true;
    // powerVR only
    bool support_pvrt = false;
    // other color format
    bool support_x8d24 = false;
    bool support_e5rgb9 = false;
    bool support_a2rgb10 = false;

    bool init(const bool hashless_texture_cache, const fs::path &texture_folder, const std::string_view game_id, const size_t sampler_cache_size = 0);
    void set_replacement_state(bool import_textures, bool export_textures, bool export_as_png);

    // The game's TextureOverride (null: none). Set it once, before the game's textures load; the
    // front end keeps it alive. After it changes what it makes, call refresh_texture_override (any
    // thread): every texture is uploaded again at its next use.
    std::atomic<TextureOverride *> texture_override{ nullptr };
    void refresh_texture_override() { texture_override_changed = true; }

    // Texture replacement from a front end, safe from any thread: the render thread applies the
    // change at its next texture bind, reads the import folder again (refresh_available_textures)
    // and uploads every texture again. request_replacement_refresh: to see a texture pack's
    // changes without a restart. set_replacement_folders: where to import from and export to,
    // in place of the shared path's textures/import|export/<game id> chosen at init.
    void request_replacement_refresh() { replacement_refresh = true; }
    void set_replacement_folders(const fs::path &import, const fs::path &export_) {
        {
            std::lock_guard<std::mutex> guard(replacement_folders_mutex);
            pending_replacement_folders = std::make_pair(import, export_);
        }
        replacement_refresh = true;
    }

private:
    std::atomic<bool> replacement_refresh{ false };
    std::mutex replacement_folders_mutex;
    std::optional<std::pair<fs::path, fs::path>> pending_replacement_folders;
    void apply_replacement_refresh();

public:

    virtual void select(size_t index, const SceGxmTexture &texture) = 0;
    virtual void configure_texture(const SceGxmTexture &texture) = 0;
    virtual void upload_texture_impl(SceGxmTextureBaseFormat base_format, uint32_t width, uint32_t height, uint32_t mip_index, const void *pixels, int face, uint32_t pixels_per_stride) = 0;
    virtual void upload_done() {}

    virtual void configure_sampler(size_t index, const SceGxmTexture &texture, bool no_linear) {}

    void upload_texture(const SceGxmTexture &gxm_texture, MemState &mem);
    void cache_and_bind_texture(const SceGxmTexture &gxm_texture, MemState &mem);

    // is called by cache_and_bind_texture if use_sampler_cache is set to true
    int cache_and_bind_sampler(const SceGxmTexture &gxm_texture, bool is_depth = false);

    // look at the texture folder and update the available imported / exported hashes
    void refresh_available_textures();

    // functions used for texture exportation
    void export_select(const SceGxmTexture &texture);
    void export_texture_impl(SceGxmTextureBaseFormat base_format, uint32_t width, uint32_t height, uint32_t mip_index, const void *pixels, int face, uint32_t pixels_per_stride);
    void export_done();

    // return false if there was an issue with the replacement texture
    bool import_configure_texture();
    // the end of import_configure_texture: the texture's parameters, reconfigured when they changed
    bool import_configure_finish(SceGxmTextureBaseFormat base_format, uint32_t width, uint32_t height, bool is_srgb, uint32_t nb_comp, uint16_t mipcount, bool swap_rb);
    virtual void import_configure_impl(SceGxmTextureBaseFormat base_format, uint32_t width, uint32_t height, bool is_srgb, uint16_t nb_components, uint16_t mipcount, bool swap_rb) = 0;
    void import_upload_texture();
    void import_done();

    virtual ~TextureCache();
};
} // namespace renderer
