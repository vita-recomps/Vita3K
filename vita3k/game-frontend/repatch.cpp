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

#include "repatch.h"
#include "packaged_mods.h"

#include <emuenv/state.h>
#include <io/functions.h>
#include <io/state.h>
#include <util/log.h>
#include <util/string_utils.h>

#include <fmt/ranges.h>
#include <miniz.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <regex>

namespace game_frontend {

namespace {

bool is_title_id(const std::string &name) {
    static const std::regex id("[A-Z]{4}[0-9]{5}");
    return std::regex_match(name, id);
}

bool is_repatch_dir(const fs::path &dir) {
    return string_utils::tolower(fs_utils::path_to_utf8(dir.filename())) == "repatch";
}

// <title ID> -> the folder holding its files: path/rePatch/<id>/ a few levels down, or path is
// rePatch/ itself, or rePatch/<id>/
std::map<std::string, fs::path> repatch_folders(const fs::path &path) {
    std::map<std::string, fs::path> found;
    const auto take = [&found](const fs::path &repatch) {
        boost::system::error_code ec;
        for (fs::directory_iterator it(repatch, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = fs_utils::path_to_utf8(it->path().filename());
            if (fs::is_directory(it->path()) && is_title_id(name))
                found.emplace(name, it->path());
        }
    };
    if (is_title_id(fs_utils::path_to_utf8(path.filename())) && is_repatch_dir(path.parent_path())) {
        found.emplace(fs_utils::path_to_utf8(path.filename()), path);
        return found;
    }
    if (is_repatch_dir(path)) {
        take(path);
        return found;
    }
    boost::system::error_code ec;
    for (fs::recursive_directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
        if (it.depth() > 2) {
            it.disable_recursion_pending();
            continue;
        }
        if (fs::is_directory(it->path()) && is_repatch_dir(it->path())) {
            take(it->path());
            it.disable_recursion_pending();
        }
    }
    return found;
}

// where a mod made for `id` goes: the installed one of the game's title IDs when `id` is one of
// them and not installed itself
std::string target_title(const fs::path &vita_fs, const std::string &id, const std::vector<std::string> &title_ids) {
    const auto installed = [&](const std::string &t) { return fs::exists(vita_fs / "ux0" / "app" / t / "eboot.bin"); };
    if (std::find(title_ids.begin(), title_ids.end(), id) == title_ids.end()) {
        LOG_WARN("rePatch: a mod for {}, which is not this game ({}): installed for {}, which this game doesn't read",
            id, fmt::join(title_ids, " or "), id);
        return id;
    }
    if (installed(id))
        return id;
    for (const auto &t : title_ids) {
        if (installed(t)) {
            LOG_INFO("rePatch: a mod for {}, installed for {}, the game installed here (the same build)", id, t);
            return t;
        }
    }
    return id;
}

bool copy_folder(const fs::path &from, const fs::path &to) {
    int files = 0;
    boost::system::error_code ec;
    for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        if (!fs::is_regular_file(it->path()))
            continue;
        const fs::path dst = to / it->path().lexically_relative(from);
        fs::create_directories(dst.parent_path(), ec);
        fs::copy_file(it->path(), dst, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            LOG_ERROR("rePatch: {} could not be copied to {}: {}", it->path(), dst, ec.message());
            return false;
        }
        LOG_INFO("rePatch: {} ({:.1f} MB)", fs_utils::path_to_utf8(it->path().lexically_relative(from)), fs::file_size(dst) / 1048576.0);
        files++;
    }
    if (ec) {
        LOG_ERROR("rePatch: {} could not be read: {}", from, ec.message());
        return false;
    }
    return files > 0;
}

size_t write_to_stream(void *opaque, mz_uint64, const void *buf, size_t n) {
    auto *out = static_cast<fs::ofstream *>(opaque);
    out->write(static_cast<const char *>(buf), static_cast<std::streamsize>(n));
    return *out ? n : 0;
}

// a zip's rePatch/<id>/ members, extracted; nullopt when it has none
std::optional<bool> install_zip(const fs::path &vita_fs, const fs::path &path, const std::vector<std::string> &title_ids) {
    FILE *fp = FOPEN(path.c_str(), "rb");
    if (!fp)
        return std::nullopt;
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_cfile(&zip, fp, 0, 0)) {
        fclose(fp);
        return std::nullopt;
    }
    // member index -> (title ID, path under rePatch/<id>/)
    static const std::regex member("(?:^|.*/)[Rr][Ee][Pp][Aa][Tt][Cc][Hh]/([A-Z]{4}[0-9]{5})/(.*[^/])");
    std::vector<std::tuple<mz_uint, std::string, std::string>> members;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < count; i++) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat) || mz_zip_reader_is_file_a_directory(&zip, i))
            continue;
        std::string name = stat.m_filename;
        string_utils::replace(name, "\\", "/");
        std::smatch m;
        if (std::regex_match(name, m, member))
            members.emplace_back(i, m[1].str(), m[2].str());
    }
    if (members.empty()) {
        mz_zip_reader_end(&zip);
        fclose(fp);
        return std::nullopt;
    }
    bool ok = true;
    std::map<std::string, std::string> targets;
    for (const auto &[index, id, rel] : members) {
        if (!targets.contains(id))
            targets[id] = target_title(vita_fs, id, title_ids);
        const fs::path dst = vita_fs / "ux0" / "rePatch" / targets[id] / fs_utils::utf8_to_path(rel);
        boost::system::error_code ec;
        fs::create_directories(dst.parent_path(), ec);
        fs::ofstream out(dst, std::ios::binary | std::ios::trunc);
        if (!out || !mz_zip_reader_extract_to_callback(&zip, index, write_to_stream, &out, 0) || !out) {
            LOG_ERROR("rePatch: {} could not be extracted to {}", rel, dst);
            ok = false;
            break;
        }
        LOG_INFO("rePatch: {} ({:.1f} MB)", rel, static_cast<double>(out.tellp()) / 1048576.0);
    }
    mz_zip_reader_end(&zip);
    fclose(fp);
    return ok;
}

} // namespace

std::optional<bool> install_repatch(const fs::path &vita_fs, const fs::path &path, const std::vector<std::string> &title_ids) {
    if (!fs::is_directory(path)) {
        const std::string ext = string_utils::tolower(fs_utils::path_to_utf8(path.extension()));
        return ext == ".pkg" ? std::nullopt : install_zip(vita_fs, path, title_ids);
    }
    const auto folders = repatch_folders(path);
    if (folders.empty())
        return std::nullopt;
    bool ok = true;
    for (const auto &[id, from] : folders) {
        const fs::path to = vita_fs / "ux0" / "rePatch" / target_title(vita_fs, id, title_ids);
        LOG_INFO("rePatch: installing {} into {}", from, to);
        ok = copy_folder(from, to) && ok;
    }
    return ok;
}

bool holds_game_content(const fs::path &path) {
    static const std::regex content("(?:^|.*/)(?:app|patch|addcont)/[A-Z]{4}[0-9]{5}/.*",
        std::regex::icase);
    static const std::regex standalone("(?:^|.*/)[A-Z]{4}[0-9]{5}/sce_sys/param\\.sfo", std::regex::icase);
    if (fs::is_directory(path)) {
        boost::system::error_code ec;
        for (fs::recursive_directory_iterator it(path, ec), end; !ec && it != end; it.increment(ec)) {
            if (it.depth() > 2) {
                it.disable_recursion_pending();
                continue;
            }
            const std::string name = string_utils::tolower(fs_utils::path_to_utf8(it->path().filename()));
            if (fs::is_directory(it->path()) && name == "repatch") {
                it.disable_recursion_pending();
                continue;
            }
            if (fs::is_directory(it->path()) && (name == "app" || name == "patch" || name == "addcont"))
                return true;
            if (is_title_id(fs_utils::path_to_utf8(it->path().filename()))
                && fs::is_regular_file(it->path() / "sce_sys" / "param.sfo"))
                return true;
        }
        return false;
    }
    FILE *fp = FOPEN(path.c_str(), "rb");
    if (!fp)
        return false;
    mz_zip_archive zip;
    std::memset(&zip, 0, sizeof(zip));
    bool found = false;
    if (mz_zip_reader_init_cfile(&zip, fp, 0, 0)) {
        const mz_uint count = mz_zip_reader_get_num_files(&zip);
        for (mz_uint i = 0; i < count && !found; i++) {
            mz_zip_archive_file_stat stat;
            if (!mz_zip_reader_file_stat(&zip, i, &stat))
                continue;
            std::string name = stat.m_filename;
            string_utils::replace(name, "\\", "/");
            if (string_utils::tolower('/' + name).contains("/repatch/"))
                continue;
            found = std::regex_match(name, standalone) || std::regex_match(name, content);
        }
        mz_zip_reader_end(&zip);
    }
    fclose(fp);
    return found;
}

int apply_repatch(EmuEnvState &emuenv, const std::string &title_id) {
    const fs::path root = emuenv.vita_fs_path / "ux0" / "rePatch" / title_id;
    const fs::path app = emuenv.vita_fs_path / "ux0" / "app" / title_id;
    if (!fs::is_directory(root))
        return 0;
    // packaged mods (packaged_mods.h) merged again where another mod replaced their archive
    refresh_packaged_mods(emuenv.vita_fs_path, title_id);
    int served = 0, skipped = 0;
    boost::system::error_code ec;
    for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        // the packages themselves, kept for merging again, aren't the game's files
        if (it.depth() == 0 && fs::is_directory(it->path()) && it->path().filename() == PACKAGED_MODS_DIR) {
            it.disable_recursion_pending();
            continue;
        }
        if (!fs::is_regular_file(it->path()))
            continue;
        const fs::path rel = it->path().lexically_relative(root);
        const fs::path installed = app / rel;
        if (!fs::exists(installed)) {
            LOG_WARN("rePatch: {} is not one of the game's files: left out (only replacements are read)", rel);
            skipped++;
            continue;
        }
        emuenv.io.read_overrides[read_override_key(installed)] = it->path();
        served++;
    }
    LOG_INFO("rePatch: {} file{} of {} read in place of the game's own{}", served, served == 1 ? "" : "s", root,
        skipped ? fmt::format(", {} left out", skipped) : "");
    return served;
}

} // namespace game_frontend
