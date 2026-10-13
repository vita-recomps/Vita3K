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

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>

namespace ss_delta {

namespace {

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

// lines of a member, from archive_patch3 when it has it (it overrides archive, as in the game)
std::optional<std::vector<std::string>> read_lines(const fs::path &app_dir, const std::string &path) {
    const fs::path retail = app_dir / "resource" / "data" / "pack" / "mount" / "retail";
    for (const char *archive : { "archive_patch3", "archive" }) {
        if (const auto data = read_member(retail / archive, path)) {
            std::vector<std::string> lines;
            std::string line;
            for (const uint8_t c : *data) {
                if (c == '\n') {
                    lines.push_back(line);
                    line.clear();
                } else if (c != '\r')
                    line += static_cast<char>(c);
            }
            if (!line.empty())
                lines.push_back(line);
            return lines;
        }
    }
    return std::nullopt;
}

} // namespace

bool addon_data_present(const fs::path &app_dir, std::string &why) {
    // the add-on pacts are quest_param.csv rows up to 580, the raiments costume_table.csv rows up
    // to 130 (saves.cpp ADDON_LISTS)
    const auto quests = read_lines(app_dir, "resource/boot/quest_param.csv");
    if (!quests || quests->size() < 1 + 581) {
        why = quests ? "quest_param.csv has " + std::to_string(quests->size() - 1) + " quests, the add-ons need 581 (game update 1.30)"
                     : "quest_param.csv not found in the game's archives";
        return false;
    }
    const auto costumes = read_lines(app_dir, "resource/boot/costume_table.csv");
    const bool raiments = costumes && std::any_of(costumes->begin(), costumes->end(), [](const std::string &l) { return l.starts_with("130,"); });
    if (!raiments) {
        why = "costume_table.csv lacks the add-on raiments";
        return false;
    }
    return true;
}

} // namespace ss_delta
