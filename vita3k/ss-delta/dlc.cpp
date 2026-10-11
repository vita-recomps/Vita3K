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

#include "dlc.h"

#include <miniz.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

namespace ss_delta {

namespace {

constexpr const char *TABLE = "resource/database/network/entitlement_table_us.csv";
constexpr const char *PREFIX = "UP9000-PCSA00152_00-SSC";
// "配布予定なし" (no distribution planned) in the table's Shift-JIS comments
constexpr std::string_view NOT_RELEASED = "\x94\x7a\x95\x7a\x97\x5c\x92\xe8\x82\xc8\x82\xb5";

// the .pkh key: CRC-32/BZIP2 of the lower-case path (tools/lspk.py in ss-delta)
uint32_t path_hash(const std::string &path) {
    static const auto table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i << 24;
            for (int k = 0; k < 8; k++)
                c = (c & 0x80000000) ? (c << 1) ^ 0x04C11DB7 : c << 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xffffffff;
    for (const char ch : path) {
        const auto b = static_cast<uint8_t>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch);
        c = (c << 8) ^ table[((c >> 24) ^ b) & 0xff];
    }
    return c ^ 0xffffffff;
}

uint32_t be32(const uint8_t *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

std::vector<uint8_t> read_all(const fs::path &path) {
    fs::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// a member of an LSPK archive (<base>.pkh index, <base>.pk data: zlib, or stored when csize is 0)
std::optional<std::vector<uint8_t>> read_member(const fs::path &base, const std::string &path) {
    const auto pkh = read_all(fs::path(base).replace_extension(".pkh"));
    if (pkh.size() < 4)
        return std::nullopt;
    const uint32_t count = be32(pkh.data());
    if (pkh.size() < 4 + size_t(count) * 16)
        return std::nullopt;
    const uint32_t hash = path_hash(path);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *r = pkh.data() + 4 + size_t(i) * 16;
        if (be32(r) != hash)
            continue;
        const uint32_t offset = be32(r + 4), size = be32(r + 8), csize = be32(r + 12);
        fs::ifstream pk(fs::path(base).replace_extension(".pk"), std::ios::binary);
        std::vector<uint8_t> stored(csize ? csize : size);
        pk.seekg(offset);
        if (!pk.read(reinterpret_cast<char *>(stored.data()), stored.size()))
            return std::nullopt;
        if (!csize)
            return stored;
        std::vector<uint8_t> data(size);
        mz_ulong out_size = size;
        if (mz_uncompress(data.data(), &out_size, stored.data(), csize) != MZ_OK || out_size != size)
            return std::nullopt;
        return data;
    }
    return std::nullopt;
}

} // namespace

DlcList find_dlc(const fs::path &app_dir) {
    DlcList out;
    const fs::path retail = app_dir / "resource" / "data" / "pack" / "mount" / "retail";
    std::optional<std::vector<uint8_t>> table;
    // the patch archive overrides the base one, as in the game
    for (const char *archive : { "archive_patch3", "archive" }) {
        table = read_member(retail / archive, TABLE);
        if (table) {
            out.source = archive;
            break;
        }
    }
    if (!table)
        return out;
    // "product id,box id,release version,comment" rows after a header line
    const std::string text(table->begin(), table->end());
    size_t pos = text.find('\n');
    while (pos != std::string::npos && pos + 1 < text.size()) {
        const size_t start = pos + 1;
        pos = text.find('\n', start);
        std::string line = text.substr(start, pos == std::string::npos ? std::string::npos : pos - start);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        const std::string id = line.substr(0, line.find(','));
        if (id.empty())
            continue;
        if (id.starts_with(PREFIX) && line.find(NOT_RELEASED) == std::string::npos)
            out.ids.push_back(id);
        else
            out.left_out.push_back(id);
    }
    return out;
}

} // namespace ss_delta
