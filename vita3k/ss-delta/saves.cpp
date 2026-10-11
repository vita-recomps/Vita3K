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

#include "saves.h"

#include <emuenv/app_util.h>
#include <util/string_utils.h>

#include <miniz.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <iterator>
#include <map>
#include <vector>

namespace ss_delta {

namespace {

static_assert(sizeof(SceAppUtilSaveDataSlotParam) == 844);

struct SaveFile {
    const char *name;
    size_t size;
};
// the game's files and their sizes in 1.30 (a check that a source is this game's save)
constexpr std::array<SaveFile, 2> FILES{ { { "datag0001", 979168 }, { "system", 1556 } } };

fs::path save_dir(const fs::path &vita_fs) {
    return vita_fs / "ux0" / "user" / "00" / "savedata" / "PCSA00152";
}

std::string lower_name(const std::string &path) {
    const auto slash = path.find_last_of("/\\");
    return string_utils::tolower(slash == std::string::npos ? path : path.substr(slash + 1));
}

// "datag0001.bin" -> contents, for the files of FILES (.bin and .bak) found in a folder or zip
using Found = std::map<std::string, std::vector<uint8_t>>;

bool wanted(const std::string &name, size_t size) {
    for (const auto &f : FILES)
        if ((name == std::string(f.name) + ".bin" || name == std::string(f.name) + ".bak") && size == f.size)
            return true;
    return false;
}

bool read_folder(const fs::path &folder, Found &found, std::string &message) {
    for (const auto &entry : fs::recursive_directory_iterator(folder)) {
        if (!fs::is_regular_file(entry.path()))
            continue;
        const auto name = lower_name(fs_utils::path_to_utf8(entry.path()));
        if (!wanted(name, static_cast<size_t>(fs::file_size(entry.path()))) || found.contains(name))
            continue;
        fs::ifstream in(entry.path(), std::ios::binary);
        found[name].assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    return true;
}

bool read_zip(const fs::path &zip_path, Found &found, std::string &message) {
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_file(&zip, fs_utils::path_to_utf8(zip_path).c_str(), 0)) {
        message = "cannot read the zip file " + fs_utils::path_to_utf8(zip_path);
        return false;
    }
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&zip); i++) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat) || stat.m_is_directory)
            continue;
        // only the known names, at their known sizes: nothing else in the zip is read
        const auto name = lower_name(stat.m_filename);
        if (!wanted(name, static_cast<size_t>(stat.m_uncomp_size)) || found.contains(name))
            continue;
        std::vector<uint8_t> data(static_cast<size_t>(stat.m_uncomp_size));
        if (mz_zip_reader_extract_to_mem(&zip, i, data.data(), data.size(), 0))
            found[name] = std::move(data);
    }
    mz_zip_reader_end(&zip);
    return true;
}

bool write_file(const fs::path &path, const void *data, size_t size) {
    fs::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
    return static_cast<bool>(out);
}

std::string timestamp() {
    const auto t = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H%M%S", &local);
    return buf;
}

// Vita3K's record of save slot 1, which the game's load screen lists; the game rewrites it with
// its own title and details the next time it saves
SceAppUtilSaveDataSlotParam slot_param() {
    SceAppUtilSaveDataSlotParam param{};
    std::strncpy(param.title, "Save data 1", sizeof(param.title) - 1);
    std::strncpy(param.subTitle, "Imported", sizeof(param.subTitle) - 1);
    std::strncpy(param.iconPath, "app0:/resource/data/icon/us/icon_g_us.png", sizeof(param.iconPath) - 1);
    param.sizeKiB = (FILES[0].size * 2 + FILES[1].size * 2 + 1023) / 1024;
    const auto t = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    param.modifiedTime.year = static_cast<unsigned short>(local.tm_year + 1900);
    param.modifiedTime.month = static_cast<unsigned short>(local.tm_mon + 1);
    param.modifiedTime.day = static_cast<unsigned short>(local.tm_mday);
    param.modifiedTime.hour = static_cast<unsigned short>(local.tm_hour);
    param.modifiedTime.minute = static_cast<unsigned short>(local.tm_min);
    param.modifiedTime.second = static_cast<unsigned short>(local.tm_sec);
    return param;
}

} // namespace

bool import_save(const fs::path &vita_fs, const fs::path &source, const fs::path &backup_dir, std::string &message) {
    Found found;
    if (fs::is_directory(source)) {
        if (!read_folder(source, found, message))
            return false;
    } else if (fs::is_regular_file(source)) {
        if (!read_zip(source, found, message))
            return false;
    } else {
        message = fs_utils::path_to_utf8(source) + " is neither a folder nor a zip file";
        return false;
    }
    for (const auto &f : FILES) {
        const std::string bin = std::string(f.name) + ".bin", bak = std::string(f.name) + ".bak";
        if (!found.contains(bin)) {
            message = "no " + bin + " of " + std::to_string(f.size) + " bytes in " + fs_utils::path_to_utf8(source)
                + " (is it a Soul Sacrifice Delta 1.30 save, decrypted?)";
            return false;
        }
        if (!found.contains(bak))
            found[bak] = found[bin];
    }

    const fs::path dir = save_dir(vita_fs);
    // the save there now goes to the backup folder first
    if (fs::exists(dir) && !fs::is_empty(dir)) {
        const fs::path backup = backup_dir / timestamp();
        fs::create_directories(backup);
        for (const auto &entry : fs::directory_iterator(dir)) {
            if (fs::is_regular_file(entry.path()))
                fs::copy_file(entry.path(), backup / entry.path().filename(), fs::copy_options::overwrite_existing);
        }
        message = "the previous save is in " + fs_utils::path_to_utf8(backup) + "; ";
    }
    fs::create_directories(dir);
    for (const auto &[name, data] : found) {
        if (!write_file(dir / name, data.data(), data.size())) {
            message += "writing " + name + " failed";
            return false;
        }
    }
    const auto param = slot_param();
    if (!write_file(dir / "SlotParam_1.bin", &param, sizeof(param))) {
        message += "writing SlotParam_1.bin failed";
        return false;
    }
    message += "imported into " + fs_utils::path_to_utf8(dir);
    return true;
}

namespace {

// system.bin (1556 bytes): a header {u32 1, u32 size 0x614, u32 0, u32 format 0x22, u32 check},
// then id lists, each sorted and ended by a zero (offsets and capacities from saves with and
// without the add-ons; the capacities are the gaps to the next list):
struct IdList {
    size_t offset, capacity, width; // width 2: u16 ids, 4: u32 ids
    std::vector<uint32_t> addons; // the released add-ons' ids in this list
};
const std::array<IdList, 4> ADDON_LISTS{ {
    // pacts: rows of quest_param.csv (the additional pacts I-XII, XIV and the otherworldly ones)
    { 0x14, 0x210 / 2, 2, { 449, 450, 451, 455, 456, 457, 458, 459, 460, 461, 462, 463, 467, 468, 469, 470, 471, 472, 561, 562, 563, 564, 565, 566, 567, 568, 569, 570, 571, 575, 576, 577, 578, 579, 580 } },
    // raiment and hood parts (parts_upper/lower_param.csv)
    { 0x224, 0x100 / 2, 2, { 624, 696, 708, 4348, 4420, 4432 } },
    // further items
    { 0x324, 0x20 / 4, 4, { 502, 512, 7812, 8702, 8802 } },
    // raiments: rows of costume_table.csv (Healer, Beast, Easterner ...)
    { 0x344, (0x614 - 0x344) / 2, 2, { 112, 113, 114, 115, 116, 117, 118, 119, 130 } },
} };
constexpr size_t SYSTEM_SIZE = 1556;
constexpr size_t CHECK_OFFSET = 0x10;

// CRC-32/BZIP2 of the file with its check field zeroed
uint32_t system_check(std::vector<uint8_t> data) {
    std::fill_n(data.begin() + CHECK_OFFSET, 4, 0);
    uint32_t c = 0xffffffff;
    for (const uint8_t b : data) {
        c ^= uint32_t(b) << 24;
        for (int k = 0; k < 8; k++)
            c = (c & 0x80000000) ? (c << 1) ^ 0x04C11DB7 : c << 1;
    }
    return c ^ 0xffffffff;
}

uint32_t read_le(const std::vector<uint8_t> &d, size_t o, size_t width) {
    uint32_t v = 0;
    for (size_t i = 0; i < width; i++)
        v |= uint32_t(d[o + i]) << (8 * i);
    return v;
}

void write_le(std::vector<uint8_t> &d, size_t o, size_t width, uint32_t v) {
    for (size_t i = 0; i < width; i++)
        d[o + i] = uint8_t(v >> (8 * i));
}

} // namespace

bool unlock_addons(const fs::path &vita_fs, const fs::path &backup_dir, std::string &message) {
    const fs::path dir = save_dir(vita_fs);
    const fs::path bin = dir / "system.bin";
    if (!fs::exists(bin)) {
        message = "no save yet";
        return true;
    }
    fs::ifstream in(bin, std::ios::binary);
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    if (data.size() != SYSTEM_SIZE || read_le(data, 4, 4) != SYSTEM_SIZE || read_le(data, CHECK_OFFSET, 4) != system_check(data)) {
        message = "system.bin is not in the expected format; left alone";
        return false;
    }
    bool changed = false;
    for (const auto &list : ADDON_LISTS) {
        std::vector<uint32_t> ids;
        for (size_t i = 0; i < list.capacity; i++) {
            const uint32_t v = read_le(data, list.offset + i * list.width, list.width);
            if (v == 0)
                break;
            ids.push_back(v);
        }
        const size_t before = ids.size();
        for (const uint32_t id : list.addons)
            if (std::find(ids.begin(), ids.end(), id) == ids.end())
                ids.push_back(id);
        if (ids.size() == before)
            continue;
        if (ids.size() >= list.capacity) {
            message = "system.bin has no room for the add-ons; left alone";
            return false;
        }
        std::sort(ids.begin(), ids.end());
        for (size_t i = 0; i < ids.size(); i++)
            write_le(data, list.offset + i * list.width, list.width, ids[i]);
        changed = true;
    }
    if (!changed) {
        message = "add-ons already recorded in the save";
        return true;
    }
    write_le(data, CHECK_OFFSET, 4, system_check(data));

    const fs::path backup = backup_dir / timestamp();
    fs::create_directories(backup);
    for (const char *name : { "system.bin", "system.bak" })
        if (fs::exists(dir / name))
            fs::copy_file(dir / name, backup / name, fs::copy_options::overwrite_existing);
    for (const char *name : { "system.bin", "system.bak" }) {
        if (!write_file(dir / name, data.data(), data.size())) {
            message = std::string("writing ") + name + " failed";
            return false;
        }
    }
    message = "add-ons recorded in the save (previous system.bin in " + fs_utils::path_to_utf8(backup) + ")";
    return true;
}

bool export_save(const fs::path &vita_fs, const fs::path &dest, std::string &message) {
    const fs::path dir = save_dir(vita_fs);
    fs::create_directories(dest);
    for (const auto &f : FILES) {
        for (const char *ext : { ".bin", ".bak" }) {
            const std::string name = std::string(f.name) + ext;
            if (!fs::exists(dir / name)) {
                message = "no save to export: " + fs_utils::path_to_utf8(dir / name) + " is missing";
                return false;
            }
            fs::copy_file(dir / name, dest / name, fs::copy_options::overwrite_existing);
        }
    }
    message = "exported to " + fs_utils::path_to_utf8(dest);
    return true;
}

} // namespace ss_delta
