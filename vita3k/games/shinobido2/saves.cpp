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
#include <util/log.h>
#include <util/string_utils.h>

#include <miniz.h>

#include <cctype>
#include <cstring>
#include <ctime>
#include <iterator>
#include <map>
#include <vector>

namespace shinobido2 {

namespace {

static_assert(sizeof(SceAppUtilSaveDataSlotParam) == 844);

constexpr size_t DATA_SIZE = 245152;
constexpr int SLOTS = 10; // 0-9

std::string data_name(int slot) {
    return fmt::format("data{:04}.bin", slot);
}

std::string param_name(int slot) {
    return fmt::format("slotparam_{}.bin", slot); // lower case, as names are compared
}

fs::path save_dir(const fs::path &vita_fs) {
    return vita_fs / "ux0" / "user" / "00" / "savedata" / "PCSE00015";
}

std::string lower_name(const std::string &path) {
    const auto slash = path.find_last_of("/\\");
    return string_utils::tolower(slash == std::string::npos ? path : path.substr(slash + 1));
}

using Found = std::map<std::string, std::vector<uint8_t>>;

// the slot of a data<NNNN>.bin or SlotParam_<N>.bin name, or -1
int slot_of(const std::string &name) {
    if (name.size() == 12 && name.starts_with("data000") && name.ends_with(".bin") && std::isdigit(static_cast<unsigned char>(name[7])))
        return name[7] - '0';
    if (name.size() == 16 && name.starts_with("slotparam_") && name.ends_with(".bin") && std::isdigit(static_cast<unsigned char>(name[10])))
        return name[10] - '0';
    return -1;
}

bool wanted(const std::string &name, size_t size) {
    if (slot_of(name) < 0)
        return false;
    return size == (name.starts_with("data") ? DATA_SIZE : sizeof(SceAppUtilSaveDataSlotParam));
}

void read_folder(const fs::path &folder, Found &found) {
    for (const auto &entry : fs::recursive_directory_iterator(folder)) {
        if (!fs::is_regular_file(entry.path()))
            continue;
        const auto name = lower_name(fs_utils::path_to_utf8(entry.path()));
        if (!wanted(name, static_cast<size_t>(fs::file_size(entry.path()))) || found.contains(name))
            continue;
        fs::ifstream in(entry.path(), std::ios::binary);
        found[name].assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
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

std::tm local_now() {
    const auto t = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    return local;
}

std::string timestamp() {
    const std::tm local = local_now();
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d_%H%M%S", &local);
    return buf;
}

// a slot record for a save from a Vita: the game writes its own (play time, day) at the next save
SceAppUtilSaveDataSlotParam slot_param() {
    SceAppUtilSaveDataSlotParam param{};
    std::strncpy(param.title, "Shinobido 2", sizeof(param.title) - 1);
    std::strncpy(param.subTitle, "Imported", sizeof(param.subTitle) - 1);
    std::strncpy(param.iconPath, "app0:/savedata/icon/icon_vs_save.png", sizeof(param.iconPath) - 1);
    param.sizeKiB = static_cast<SceSize>((DATA_SIZE + 1023) / 1024);
    const std::tm local = local_now();
    param.modifiedTime.year = static_cast<unsigned short>(local.tm_year + 1900);
    param.modifiedTime.month = static_cast<unsigned short>(local.tm_mon + 1);
    param.modifiedTime.day = static_cast<unsigned short>(local.tm_mday);
    param.modifiedTime.hour = static_cast<unsigned short>(local.tm_hour);
    param.modifiedTime.minute = static_cast<unsigned short>(local.tm_min);
    param.modifiedTime.second = static_cast<unsigned short>(local.tm_sec);
    return param;
}

} // namespace

bool import_save(const fs::path &vita_fs, const fs::path &source, int slot, const fs::path &backup_dir, std::string &message) {
    if (slot < -1 || slot >= SLOTS) {
        message = fmt::format("there is no slot {}: the game has slots 0 to {}", slot, SLOTS - 1);
        return false;
    }
    Found found;
    if (fs::is_directory(source))
        read_folder(source, found);
    else if (fs::is_regular_file(source)) {
        if (!read_zip(source, found, message))
            return false;
    } else {
        message = fs_utils::path_to_utf8(source) + " is neither a folder nor a zip file";
        return false;
    }
    std::vector<int> data_slots; // the source's data files, by their number
    for (const auto &[name, _] : found)
        if (name.starts_with("data"))
            data_slots.push_back(slot_of(name));
    if (data_slots.empty()) {
        message = fmt::format("no data0000.bin-data0009.bin of {} bytes in {} (is it a Shinobido 2 save, decrypted?)", DATA_SIZE,
            fs_utils::path_to_utf8(source));
        return false;
    }
    // (source slot, target slot) pairs
    std::vector<std::pair<int, int>> moves;
    if (slot < 0) {
        for (const int s : data_slots)
            moves.emplace_back(s, s);
    } else if (data_slots.size() == 1)
        moves.emplace_back(data_slots[0], slot);
    else if (found.contains(data_name(slot)))
        moves.emplace_back(slot, slot);
    else {
        message = fs_utils::path_to_utf8(source) + fmt::format(" holds {} saves but no {}; name one with --slot", data_slots.size(), data_name(slot));
        return false;
    }

    const fs::path dir = save_dir(vita_fs);
    // the save folder as it is goes to the backup folder first
    if (fs::exists(dir) && !fs::is_empty(dir)) {
        // a folder of its own, also for a second import in the same second
        fs::path backup = backup_dir / timestamp();
        for (int n = 2; fs::exists(backup); n++)
            backup = backup_dir / (timestamp() + "_" + std::to_string(n));
        fs::create_directories(backup);
        for (const auto &entry : fs::directory_iterator(dir)) {
            if (fs::is_regular_file(entry.path()))
                fs::copy_file(entry.path(), backup / entry.path().filename(), fs::copy_options::overwrite_existing);
        }
        message = "the save folder as it was is in " + fs_utils::path_to_utf8(backup) + "; ";
    }
    fs::create_directories(dir);
    std::vector<std::string> done;
    for (const auto &[from, to] : moves) {
        const auto &data = found.at(data_name(from));
        bool ok = write_file(dir / data_name(to), data.data(), data.size());
        if (found.contains(param_name(from))) {
            const auto &param = found.at(param_name(from));
            ok = ok && write_file(dir / fmt::format("SlotParam_{}.bin", to), param.data(), param.size());
        } else {
            const auto param = slot_param();
            ok = ok && write_file(dir / fmt::format("SlotParam_{}.bin", to), &param, sizeof(param));
        }
        if (!ok) {
            message += fmt::format("writing slot {} failed", to);
            return false;
        }
        done.push_back(from == to ? std::to_string(to) : fmt::format("{} (from {})", to, data_name(from)));
    }
    message += fmt::format("imported into slot{} {} ({})", done.size() > 1 ? "s" : "", fmt::join(done, ", "), fs_utils::path_to_utf8(dir));
    return true;
}

bool export_save(const fs::path &vita_fs, const fs::path &dest, int slot, std::string &message) {
    if (slot < -1 || slot >= SLOTS) {
        message = fmt::format("there is no slot {}: the game has slots 0 to {}", slot, SLOTS - 1);
        return false;
    }
    const fs::path dir = save_dir(vita_fs);
    std::vector<int> slots;
    for (int s = 0; s < SLOTS; s++)
        if ((slot < 0 || s == slot) && fs::exists(dir / data_name(s)))
            slots.push_back(s);
    if (slots.empty()) {
        message = slot < 0 ? "there is no save yet" : fmt::format("slot {} is empty", slot);
        return false;
    }
    boost::system::error_code ec;
    fs::create_directories(dest, ec);
    for (const int s : slots) {
        fs::copy_file(dir / data_name(s), dest / data_name(s), fs::copy_options::overwrite_existing, ec);
        const fs::path param = dir / fmt::format("SlotParam_{}.bin", s);
        if (!ec && fs::exists(param))
            fs::copy_file(param, dest / param.filename(), fs::copy_options::overwrite_existing, ec);
        if (ec) {
            message = fmt::format("copying slot {} to {} failed: {}", s, fs_utils::path_to_utf8(dest), ec.message());
            return false;
        }
    }
    message = fmt::format("exported slot{} {} to {}", slots.size() > 1 ? "s" : "", fmt::join(slots, ", "), fs_utils::path_to_utf8(dest));
    return true;
}

} // namespace shinobido2
