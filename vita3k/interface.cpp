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

#include "interface.h"
#include "archive.h"

#include "module/load_module.h"

#include <config/state.h>
#include <ctime>
#include <ctrl/state.h>
#include <dialog/state.h>
#include <display/functions.h>
#include <display/state.h>
#include <emuenv/state.h>
#include <io/functions.h>
#include <io/vfs.h>
#include <kernel/state.h>
#include <lang/state.h>
#include <packages/pkg.h>
#include <packages/sfo.h>
#include <packages/vci.h>
#include <renderer/state.h>
#include <renderer/texture_cache.h>

#include <miniz.h>
#include <pugixml.hpp>

#include <modules/module_parent.h>
#include <string>
#include <util/log.h>
#include <util/string_utils.h>
#include <util/vector_utils.h>
#include <util/vita_theme_utils.h>

#include <gdbstub/functions.h>
#include <stb_image_write.h>

#if USE_DISCORD
#include <app/discord.h>
#endif

#include "patch/patch.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <memory>
#include <mutex>
#include <regex>

typedef std::shared_ptr<mz_zip_archive> ZipPtr;

inline void delete_zip(mz_zip_archive *zip) {
    mz_zip_reader_end(zip);
    delete zip;
}

static size_t write_to_buffer(void *pOpaque, mz_uint64 file_ofs, const void *pBuf, size_t n) {
    vfs::FileBuffer *const buffer = static_cast<vfs::FileBuffer *>(pOpaque);
    assert(file_ofs == buffer->size());
    const uint8_t *const first = static_cast<const uint8_t *>(pBuf);
    const uint8_t *const last = &first[n];
    buffer->insert(buffer->end(), first, last);

    return n;
}

static const char *miniz_get_error(const ZipPtr &zip) {
    return mz_zip_get_error_string(mz_zip_get_last_error(zip.get()));
}

static std::string fallback_theme_root_name(const std::string &content_path) {
    std::string trimmed = content_path;
    while (!trimmed.empty() && ((trimmed.back() == '/') || (trimmed.back() == '\\')))
        trimmed.pop_back();

    if (trimmed.empty())
        return {};

    const auto separator = trimmed.find_last_of("/\\");
    return (separator == std::string::npos) ? trimmed : trimmed.substr(separator + 1);
}

// A licence's kind, from its account ID (0x08). NoNpDrm's licences (and the work.bin made from a zRIF) carry
// the fake 0x0123456789ABCDEF and the content's key in the clear. The licence a Vita keeps with a game it
// installed (its sce_sys/package/work.bin, ux0:license) carries the PSN account's ID and a key that only
// that console can decrypt: nothing outside the Vita can use it (ss-delta #13).
enum class LicenseKind {
    unknown,
    nonpdrm,
    account
};

static LicenseKind license_kind(const fs::path &rif) {
    fs::ifstream file(rif, std::ios::binary);
    std::array<char, 0x200> data{};
    if (!file || !file.read(data.data(), data.size()))
        return LicenseKind::unknown;
    static constexpr unsigned char fake_aid[8] = { 0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01 };
    return std::memcmp(data.data() + 8, fake_aid, sizeof(fake_aid)) == 0 ? LicenseKind::nonpdrm : LicenseKind::account;
}

static bool is_nonpdrm(EmuEnvState &emuenv, const fs::path &output_path) {
    const auto app_license_path{ emuenv.vita_fs_path / "ux0/license" / emuenv.app_info.app_title_id / fmt::format("{}.rif", emuenv.app_info.app_content_id) };
    const auto is_patch_found_app_license = (emuenv.app_info.app_category == "gp") && fs::exists(app_license_path);
    if (fs::exists(output_path / "sce_sys/package/work.bin") || is_patch_found_app_license) {
        fs::path licpath = is_patch_found_app_license ? app_license_path : output_path / "sce_sys/package/work.bin";
        LOG_INFO("Decrypt layer: {}", output_path);
        if (!decrypt_install_nonpdrm(emuenv, licpath, output_path)) {
            if (license_kind(licpath) == LicenseKind::account)
                LOG_ERROR("{} [{}]: its licence is the PSN account's own, as a Vita keeps it with a game it installed. "
                          "Its key works only on that Vita, so the files can't be decrypted here. Make a NoNpDrm dump: "
                          "with the NoNpDrm plugin loaded, start the game once on the Vita, and it writes a licence "
                          "that works anywhere to ux0:nonpdrm/license/ (app/{} for the game, addcont/{}/<add-on> for "
                          "an add-on). Copy that license folder next to app/ and patch/ in the dump, or give the .rif "
                          "as the game's licence",
                    emuenv.app_info.app_title, emuenv.app_info.app_title_id, emuenv.app_info.app_title_id,
                    emuenv.app_info.app_title_id);
            LOG_ERROR("NoNpDrm installation failed, deleting data!");
            fs::remove_all(output_path);
            return false;
        }
        return true;
    }

    return false;
}

// A NoNpDrm dump laid out as on a Vita's ux0: keeps an add-on's or a game's licence outside the content,
// in license/addcont/<title>/<dir>/*.rif or license/app/<title>/*.rif, where pkg2zip dumps have
// sce_sys/package/work.bin. Both paths below are relative to the dump's root: where the content sits
// (addcont/<title>/<dir>/ or app/<title>/) and where its licence would be; empty for other content.
// Called after set_content_path, which cuts an add-on's content ID down to its folder name.
struct DumpLicenseDirs {
    std::string content;
    std::string license;
};

static DumpLicenseDirs dump_license_dirs(const EmuEnvState &emuenv) {
    const auto &info = emuenv.app_info;
    if (info.app_category == "ac")
        return { fmt::format("addcont/{}/{}/", info.app_title_id, info.app_content_id),
            fmt::format("license/addcont/{}/{}/", info.app_title_id, info.app_content_id) };
    if (info.app_category.starts_with("gd"))
        return { fmt::format("app/{}/", info.app_title_id), fmt::format("license/app/{}/", info.app_title_id) };
    return {};
}

// The content's work.bin is usable or missing: false when it is an account's licence (a folder copied
// from a Vita's ux0:app), which a NoNpDrm licence beside the content then replaces.
static bool keep_work_bin(const fs::path &work_bin) {
    if (!fs::exists(work_bin))
        return false;
    if (license_kind(work_bin) != LicenseKind::account)
        return true;
    LOG_WARN("{}: the PSN account's licence, usable only on the Vita it came from; looking for a NoNpDrm licence "
             "beside the dump",
        work_bin);
    return false;
}

// The game's licence given with the dump (--license, --zrif): its work.bin, in place of the dump's own.
static void take_given_license(EmuEnvState &emuenv, const fs::path &license, const fs::path &output_path) {
    if (license.empty() || !emuenv.app_info.app_category.starts_with("gd"))
        return;
    const auto work_bin = output_path / "sce_sys/package/work.bin";
    boost::system::error_code ec;
    fs::create_directories(work_bin.parent_path(), ec);
    fs::copy_file(license, work_bin, fs::copy_options::overwrite_existing, ec);
    if (ec)
        LOG_ERROR("Licence {} could not be copied: {}", license, ec.message());
    else
        LOG_INFO("Licence: {} (given with the dump)", license);
}

// Without a usable work.bin in the installed content: the licence a zip keeps beside it, written as its work.bin.
static void take_zip_dump_license(EmuEnvState &emuenv, const ZipPtr &zip, const std::string &content_path, const fs::path &output_path) {
    const auto dirs = dump_license_dirs(emuenv);
    const auto work_bin = output_path / "sce_sys/package/work.bin";
    if (dirs.license.empty() || !content_path.ends_with(dirs.content) || keep_work_bin(work_bin))
        return;
    const auto prefix = content_path.substr(0, content_path.size() - dirs.content.size()) + dirs.license;
    const mz_uint num_files = mz_zip_reader_get_num_files(zip.get());
    for (mz_uint i = 0; i < num_files; i++) {
        mz_zip_archive_file_stat file_stat;
        if (!mz_zip_reader_file_stat(zip.get(), i, &file_stat) || mz_zip_reader_is_file_a_directory(zip.get(), i))
            continue;
        const std::string name = file_stat.m_filename;
        if (!name.starts_with(prefix) || !string_utils::tolower(name).ends_with(".rif"))
            continue;
        fs::create_directories(work_bin.parent_path());
        if (mz_zip_reader_extract_to_file(zip.get(), i, fs_utils::path_to_utf8(work_bin).c_str(), 0)) {
            LOG_INFO("Licence: {}", name);
            return;
        }
    }
}

// The same for a dumped folder: the licence next to the content's own folders, copied in as its work.bin.
static void take_folder_dump_license(EmuEnvState &emuenv, const fs::path &content_path, const fs::path &dst_path) {
    const auto dirs = dump_license_dirs(emuenv);
    const auto work_bin = dst_path / "sce_sys/package/work.bin";
    if (dirs.license.empty() || keep_work_bin(work_bin))
        return;
    // the dump's root: up one folder per part of dirs.content (addcont/<title>/<dir>/ or app/<title>/)
    fs::path root = content_path;
    for (const char c : dirs.content)
        if (c == '/')
            root = root.parent_path();
    boost::system::error_code ec;
    if (!fs::equivalent(root / fs_utils::utf8_to_path(dirs.content), content_path, ec) || ec)
        return;
    const auto license_dir = root / fs_utils::utf8_to_path(dirs.license);
    if (!fs::is_directory(license_dir, ec))
        return;
    for (const auto &entry : fs::directory_iterator(license_dir, ec)) {
        if (!fs::is_regular_file(entry.path(), ec) || string_utils::tolower(entry.path().extension().string()) != ".rif")
            continue;
        fs::create_directories(work_bin.parent_path(), ec);
        fs::copy_file(entry.path(), work_bin, fs::copy_options::overwrite_existing, ec);
        if (!ec) {
            LOG_INFO("Licence: {}", entry.path());
            return;
        }
    }
}

// Content that kept its PFS index (sce_pfs) was installed as dumped, not decrypted: its files are most
// likely still encrypted, which the game can't read.
static void warn_if_still_encrypted(const EmuEnvState &emuenv, const fs::path &installed_path) {
    if (!fs::exists(installed_path / "sce_pfs/files.db"))
        return;
    const auto dirs = dump_license_dirs(emuenv);
    LOG_WARN("{} [{}] was installed as it is, and its files look encrypted (sce_pfs is still there): no licence "
             "was found (sce_sys/package/work.bin{}). The game can't read encrypted files: install it again with "
             "its licence (a pkg: --license or --zrif)",
        emuenv.app_info.app_title, emuenv.app_info.app_title_id,
        dirs.license.empty() ? "" : fmt::format(", or {}*.rif beside {}", dirs.license, dirs.content));
}

static bool set_content_path(EmuEnvState &emuenv, const bool is_theme, fs::path &dest_path) {
    if (emuenv.app_info.app_title_id.empty()) {
        LOG_ERROR("param.sfo has no title ID, not installing it");
        return false;
    }

    const auto app_path = dest_path / "app" / emuenv.app_info.app_title_id;

    if (emuenv.app_info.app_category == "ac") {
        if (is_theme) {
            dest_path /= fs::path("theme") / emuenv.app_info.app_content_id;
            emuenv.app_info.app_title += " (Theme)";
        } else {
            emuenv.app_info.app_content_id = emuenv.app_info.app_content_id.substr(20);
            dest_path /= fs::path("addcont") / emuenv.app_info.app_title_id / emuenv.app_info.app_content_id;
            emuenv.app_info.app_title += " (DLC)";
        }
    } else if (emuenv.app_info.app_category.contains("gp")) {
        if (!fs::exists(app_path) || fs::is_empty(app_path)) {
            LOG_ERROR("Install app before patch: this is only the update (patch) {} {}, and the game itself isn't "
                      "installed. Install the game first (NoNpDrm dumps keep it in app/{} and the update in patch/{}; "
                      "the update unpacked over the game's folder doesn't work)",
                emuenv.app_info.app_title_id, emuenv.app_info.app_version, emuenv.app_info.app_title_id, emuenv.app_info.app_title_id);
            return false;
        }
        dest_path /= fs::path("patch") / emuenv.app_info.app_title_id;
        emuenv.app_info.app_title += " (Patch)";
    } else {
        dest_path = app_path;
        emuenv.app_info.app_title += " (App)";
    }

    return true;
}

static void set_theme_name(EmuEnvState &emuenv, const vfs::FileBuffer &buffer, const std::string &fallback_id_hint = {}) {
    std::string content_id;
    std::string title;

    pugi::xml_document doc;
    if (doc.load_buffer(buffer.data(), buffer.size())) {
        const auto info = doc.child("theme").child("InfomationProperty");
        content_id = info.child("m_contentId").text().as_string();
        title = info.child("m_title").child("m_default").text().as_string();
    } else {
        LOG_WARN("Unable to parse theme.xml metadata during install, falling back to folder/title-derived theme identity");
    }

    const std::string resolved_id = vita_theme_utils::resolve_theme_id(
        content_id,
        fallback_id_hint,
        title,
        buffer.empty() ? nullptr : buffer.data(),
        buffer.size());
    emuenv.app_info.app_content_id = resolved_id;
    emuenv.app_info.app_title_id = resolved_id;

    if (!title.empty()) {
        emuenv.app_info.app_title = title;
    } else if (emuenv.app_info.app_title.empty()) {
        emuenv.app_info.app_title = resolved_id;
    }
}

// A dump's contents in the order they install in: the game, then its update, then the rest (add-ons,
// themes). An update before its game fails ("Install app before patch"), and a folder's or zip's own order
// is arbitrary: Linux lists a folder in its file system's order, which put patch/ before app/ (ss-delta #13).
template <typename Content, typename ReadSfo>
static void sort_contents(std::vector<Content> &contents, const ReadSfo &read_sfo) {
    std::vector<std::pair<int, Content>> ranked;
    for (const auto &content : contents) {
        const vfs::FileBuffer sfo = read_sfo(content);
        int rank = 2;
        if (!sfo.empty()) {
            sfo::SfoAppInfo info;
            sfo::get_param_info(info, sfo, 0);
            if (info.app_category.starts_with("gd"))
                rank = 0;
            else if (info.app_category.starts_with("gp"))
                rank = 1;
        }
        ranked.emplace_back(rank, content);
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    for (size_t i = 0; i < ranked.size(); i++)
        contents[i] = ranked[i].second;
}

static bool install_archive_content(EmuEnvState &emuenv, const ZipPtr &zip, const std::string &content_path, const std::function<void(ArchiveContents)> &progress_callback, const ReinstallCallback &reinstall_callback, const fs::path &license) {
    std::string sfo_path = "sce_sys/param.sfo";
    std::string theme_path = "theme.xml";
    vfs::FileBuffer buffer, theme;

    const auto is_theme = mz_zip_reader_extract_file_to_callback(zip.get(), (content_path + theme_path).c_str(), &write_to_buffer, &theme, 0);
    const std::string theme_root_name = fallback_theme_root_name(content_path);

    auto output_path{ emuenv.vita_fs_path / "ux0" };
    if (mz_zip_reader_extract_file_to_callback(zip.get(), (content_path + sfo_path).c_str(), &write_to_buffer, &buffer, 0)) {
        sfo::get_param_info(emuenv.app_info, buffer, emuenv.cfg.sys_lang);
        if (!set_content_path(emuenv, is_theme, output_path))
            return false;
    } else if (is_theme) {
        set_theme_name(emuenv, theme, theme_root_name);
        output_path /= fs::path("theme") / emuenv.app_info.app_content_id;
    } else {
        LOG_CRITICAL("miniz error: {} extracting file: {}", miniz_get_error(zip), sfo_path);
        return false;
    }

    const auto created = fs::create_directories(output_path);
    if (!created) {
        if (reinstall_callback) {
            if (!reinstall_callback(emuenv.app_info.app_title, emuenv.app_info.app_title_id)) {
                LOG_INFO("{} already installed, skipping", emuenv.app_info.app_title_id);
                return true;
            }
        }
        fs::remove_all(output_path);
    }

    float file_progress = 0;
    float decrypt_progress = 0;

    const auto update_progress = [&]() {
        if (progress_callback)
            progress_callback({ {}, {}, { file_progress * 0.7f + decrypt_progress * 0.3f } });
    };

    mz_uint num_files = mz_zip_reader_get_num_files(zip.get());
    for (mz_uint i = 0; i < num_files; i++) {
        mz_zip_archive_file_stat file_stat;
        if (!mz_zip_reader_file_stat(zip.get(), i, &file_stat)) {
            continue;
        }
        const std::string m_filename = file_stat.m_filename;
        // starts_with: a ux0-style dump's license/addcont/<title>/<dir>/ holds this content's path too
        if (m_filename.starts_with(content_path)) {
            file_progress = static_cast<float>(i) / num_files * 100.0f;
            update_progress();

            std::string replace_filename = m_filename.substr(content_path.size());
            const fs::path file_output = (output_path / fs_utils::utf8_to_path(replace_filename)).generic_path();
            if (mz_zip_reader_is_file_a_directory(zip.get(), i)) {
                fs::create_directories(file_output);
            } else {
                fs::create_directories(file_output.parent_path());
                LOG_INFO("Extracting {}", file_output);
                mz_zip_reader_extract_to_file(zip.get(), i, fs_utils::path_to_utf8(file_output).c_str(), 0);
            }
        }
    }

    take_given_license(emuenv, license, output_path);
    take_zip_dump_license(emuenv, zip, content_path, output_path);
    if (fs::exists(output_path / "sce_sys/package/") && emuenv.app_info.app_title_id.starts_with("PCS")) {
        update_progress();
        if (is_nonpdrm(emuenv, output_path))
            decrypt_progress = 100.f;
        else
            return false;
    }
    warn_if_still_encrypted(emuenv, output_path);
    if (!copy_path(output_path, emuenv.vita_fs_path, emuenv.app_info.app_title_id, emuenv.app_info.app_category))
        return false;

    update_progress();

    LOG_INFO("{} [{}] installed successfully!", emuenv.app_info.app_title, emuenv.app_info.app_title_id);

    return true;
}

static std::vector<std::string> get_archive_contents_path(const ZipPtr &zip) {
    mz_uint num_files = mz_zip_reader_get_num_files(zip.get());
    std::vector<std::string> content_path;
    std::string sfo_path = "sce_sys/param.sfo";
    std::string theme_path = "theme.xml";

    for (mz_uint i = 0; i < num_files; i++) {
        mz_zip_archive_file_stat file_stat;
        if (!mz_zip_reader_file_stat(zip.get(), i, &file_stat))
            continue;

        std::string m_filename = std::string(file_stat.m_filename);
        std::string normalized = m_filename;
        string_utils::replace(normalized, "\\", "/");
        if (string_utils::tolower('/' + normalized).contains("/repatch/"))
            continue;
        if (m_filename.contains("sce_module/steroid.suprx")) {
            LOG_CRITICAL("A Vitamin dump was detected, aborting installation...");
#ifdef __ANDROID__
            // SDL_ShowAndroidToast("Vitamin dumps are not supported!", 1, -1, 0, 0);
#endif
            content_path.clear();
            break;
        }

        const auto is_content = m_filename.contains(sfo_path) || m_filename.contains(theme_path);
        if (is_content) {
            const auto content_type = m_filename.contains(sfo_path) ? sfo_path : theme_path;
            m_filename.erase(m_filename.find(content_type));
            vector_utils::push_if_not_exists(content_path, m_filename);
        }
    }

    return content_path;
}

std::vector<ContentInfo> install_archive(EmuEnvState &emuenv, const fs::path &archive_path, const std::function<void(ArchiveContents)> &progress_callback, const ReinstallCallback &reinstall_callback, const fs::path &license) {
    if (string_utils::tolower(archive_path.extension().string()) == ".vci") {
        const auto vci_progress = [&](float pct) {
            if (progress_callback)
                progress_callback({ 1.f, 1.f, pct });
        };
        const bool state = install_vci(archive_path, emuenv, vci_progress);
        std::vector<ContentInfo> content_installed{};
        content_installed.push_back({ emuenv.app_info.app_title, emuenv.app_info.app_title_id, emuenv.app_info.app_category, emuenv.app_info.app_content_id, archive_path.string(), state });
        return content_installed;
    }

    FILE *vpk_fp = FOPEN(archive_path.c_str(), "rb");
    if (!vpk_fp) {
        LOG_CRITICAL("Failed to load archive file in path: {} ({})", fs_utils::path_to_utf8(archive_path), std::strerror(errno));
        return {};
    }

    const ZipPtr zip(new mz_zip_archive, delete_zip);
    std::memset(zip.get(), 0, sizeof(*zip));

    if (!mz_zip_reader_init_cfile(zip.get(), vpk_fp, 0, 0)) {
        LOG_CRITICAL("miniz error reading archive: {}", miniz_get_error(zip));
        fclose(vpk_fp);
        return {};
    }

    auto content_path = get_archive_contents_path(zip);
    if (content_path.empty()) {
        fclose(vpk_fp);
        return {};
    }
    sort_contents(content_path, [&zip](const std::string &content) {
        vfs::FileBuffer sfo;
        if (!mz_zip_reader_extract_file_to_callback(zip.get(), (content + "sce_sys/param.sfo").c_str(), &write_to_buffer, &sfo, 0))
            sfo.clear();
        return sfo;
    });

    const auto count = static_cast<float>(content_path.size());
    float current = 0.f;
    const auto update_progress = [&]() {
        if (progress_callback)
            progress_callback({ count, current, {} });
    };
    update_progress();

    std::vector<ContentInfo> content_installed{};
    for (auto &path : content_path) {
        current++;
        update_progress();
        bool state = install_archive_content(emuenv, zip, path, progress_callback, reinstall_callback, license);
        // Can't use emplace_back due to Clang 15 for macos
        content_installed.push_back({ emuenv.app_info.app_title, emuenv.app_info.app_title_id, emuenv.app_info.app_category, emuenv.app_info.app_content_id, path, state });
    }

    fclose(vpk_fp);
    return content_installed;
}

static std::vector<fs::path> get_contents_path(const fs::path &path) {
    std::vector<fs::path> contents_path;

    for (fs::recursive_directory_iterator it(path), end; it != end; ++it) {
        auto filename = it->path().filename();
        if (fs::is_directory(it->path()) && string_utils::tolower(fs_utils::path_to_utf8(filename)) == "repatch") {
            it.disable_recursion_pending();
            continue;
        }
        const auto is_content = (filename == "param.sfo") || (filename == "theme.xml");
        if (is_content) {
            auto parent_path = it->path().parent_path();
            const auto content_path = (filename == "param.sfo") ? parent_path.parent_path() : parent_path;
            vector_utils::push_if_not_exists(contents_path, content_path);
        }
    }

    sort_contents(contents_path, [](const fs::path &content) {
        vfs::FileBuffer sfo;
        fs_utils::read_data(content / "sce_sys/param.sfo", sfo);
        return sfo;
    });
    return contents_path;
}

static bool install_content(EmuEnvState &emuenv, const fs::path &content_path, const fs::path &license) {
    const auto sfo_path{ content_path / "sce_sys/param.sfo" };
    const auto theme_path{ content_path / "theme.xml" };
    vfs::FileBuffer buffer;

    const auto is_theme = fs::exists(theme_path);
    auto dst_path{ emuenv.vita_fs_path / "ux0" };
    if (fs_utils::read_data(sfo_path, buffer)) {
        sfo::get_param_info(emuenv.app_info, buffer, emuenv.cfg.sys_lang);
        if (!set_content_path(emuenv, is_theme, dst_path))
            return false;

        if (exists(dst_path))
            fs::remove_all(dst_path);

    } else if (fs_utils::read_data(theme_path, buffer)) {
        set_theme_name(emuenv, buffer, fs_utils::path_to_utf8(content_path.filename()));
        dst_path /= fs::path("theme") / emuenv.app_info.app_title_id;
    } else {
        LOG_ERROR("Param.sfo file is missing in path", sfo_path);
        return false;
    }

    if (!fs_utils::copy_directory_contents(content_path, dst_path)) {
        LOG_ERROR("Failed to copy directory to: {}", dst_path);
        return false;
    }

    take_given_license(emuenv, license, dst_path);
    take_folder_dump_license(emuenv, content_path, dst_path);
    if (fs::exists(dst_path / "sce_sys/package/") && !is_nonpdrm(emuenv, dst_path))
        return false;
    warn_if_still_encrypted(emuenv, dst_path);

    if (!copy_path(dst_path, emuenv.vita_fs_path, emuenv.app_info.app_title_id, emuenv.app_info.app_category))
        return false;

    LOG_INFO("{} [{}] installed successfully!", emuenv.app_info.app_title, emuenv.app_info.app_title_id);

    return true;
}

uint32_t install_contents(EmuEnvState &emuenv, const fs::path &path, const fs::path &license, uint32_t *found) {
    const auto src_path = get_contents_path(path);
    if (found)
        *found = static_cast<uint32_t>(src_path.size());

    LOG_WARN_IF(src_path.empty(), "No found any content compatible on this path: {}", path);

    uint32_t installed = 0;
    for (const auto &src : src_path) {
        if (install_content(emuenv, src, license))
            ++installed;
    }

    if (installed) {
        LOG_INFO("Successfully installed {} content!", installed);
    }

    return installed;
}

static void do_patches(MemState &mem, const Patches &patches, const SceKernelModuleInfo &sceKernelModuleInfo) {
    for (const auto &patch : patches) {
        if (patch.seg < MODULE_INFO_NUM_SEGMENTS) {
            auto &seg = sceKernelModuleInfo.segments[patch.seg];
            auto seg_ptr = seg.vaddr.cast<uint8_t>();
            if (seg_ptr) {
                LOG_INFO("Patching segment {} at offset 0x{:X} with {} values", patch.seg, patch.offset, patch.values.size());
                if (patch.offset + patch.values.size() <= seg.memsz) {
                    memcpy(seg_ptr.get(mem) + patch.offset, patch.values.data(), patch.values.size());
                } else {
                    LOG_ERROR("Patch out of bounds for segment {} at offset 0x{:X}", patch.seg, patch.offset);
                }
            }
        }
    }
}

static ExitCode load_app_impl(SceUID &main_module_id, EmuEnvState &emuenv, const AppLaunchRequest &launch_request) {
    const auto call_import = [&emuenv](CPUState &cpu, uint32_t nid, SceUID thread_id) {
        // VITA3K_STACK_AT=<nid hex>[,...] logs, for the first VITA3K_STACK_N (default 4) calls of each of those
        // imports, the thread, LR and the words in the 4 KB above SP that look like Thumb return addresses into
        // the eboot (0x81000000-0x81a93000): call chains for finding a game's loops (stale words included)
        static std::map<uint32_t, int> stack_at = [] {
            std::map<uint32_t, int> m;
            if (const char *v = std::getenv("VITA3K_STACK_AT")) {
                std::string s = v;
                size_t p = 0;
                while (p < s.size()) {
                    const size_t c = s.find(',', p);
                    m[std::stoul(s.substr(p, c - p), nullptr, 16)] = 0;
                    if (c == std::string::npos)
                        break;
                    p = c + 1;
                }
            }
            return m;
        }();
        static const int stack_n = std::getenv("VITA3K_STACK_N") ? std::atoi(std::getenv("VITA3K_STACK_N")) : 4;
        if (!stack_at.empty()) {
            static std::mutex stack_at_mutex;
            const auto it = stack_at.find(nid);
            const bool log = it != stack_at.end() && [&] {
                const std::lock_guard<std::mutex> lock(stack_at_mutex);
                return it->second++ < stack_n;
            }();
            if (log) {
                const uint32_t sp = read_sp(cpu);
                std::string ret;
                for (uint32_t a = sp; a < sp + 0x1000; a += 4) {
                    if ((a & 0xfff) == 0 || a == sp)
                        if (!is_valid_addr(emuenv.mem, a))
                            break;
                    const uint32_t w = *Ptr<uint32_t>(a).get(emuenv.mem);
                    if ((w & 1) && w >= 0x81000000 && w < 0x81a93000)
                        ret += fmt::format(" {:x}@{:x}", w & ~1u, a - sp);
                }
                LOG_INFO("STACK_AT nid {:08x} call {} thread {} lr {:08x}:{}", nid, it->second, thread_id, read_lr(cpu), ret);
            }
        }
        ::call_import(emuenv, cpu, nid, thread_id);
    };
    emuenv.kernel.process_exit_callback = [&emuenv](int res, std::optional<AppLaunchRequest> relaunch) {
        emuenv.post_app_launch_request(relaunch.value_or(AppLaunchRequest{ .reason = AppLaunchReason::ProcessExit, .exit_code = res }));
    };
    if (!emuenv.kernel.init(emuenv.mem, call_import, emuenv.cfg.current_config.cpu_opt)) {
        LOG_WARN("Failed to init kernel!");
        return KernelInitFailed;
    }

    if (emuenv.cfg.archive_log) {
        const fs::path log_directory{ emuenv.log_path / "logs" };
        fs::create_directory(log_directory);
        const auto log_path{ log_directory / fs_utils::utf8_to_path(emuenv.io.title_id + " - [" + string_utils::remove_special_chars(emuenv.current_app_title) + "].log") };
        if (logging::add_sink(log_path) != Success)
            return InitConfigFailed;
        logging::set_level(static_cast<spdlog::level::level_enum>(emuenv.cfg.log_level));
    }

    LOG_INFO("CPU Optimisation state: {}", emuenv.cfg.current_config.cpu_opt);
    LOG_INFO("ngs state: {}", emuenv.cfg.current_config.ngs_enable);
    LOG_INFO("Resolution multiplier: {}", emuenv.cfg.resolution_multiplier);

    if (emuenv.ctrl.controllers_num) {
        LOG_INFO("{} Controllers Connected", emuenv.ctrl.controllers_num);
        for (auto controller_it = emuenv.ctrl.controllers.begin(); controller_it != emuenv.ctrl.controllers.end(); ++controller_it) {
            LOG_INFO("Controller {}: {}", controller_it->second.port, controller_it->second.name);
        }
        if (emuenv.ctrl.has_motion_support)
            LOG_INFO("Controller has motion support");
    }
    constexpr std::array modules_mode_names{ "Automatic", "Auto & Manual", "Manual" };
    LOG_INFO("modules mode: {}", modules_mode_names.at(emuenv.cfg.current_config.modules_mode));
    if ((emuenv.cfg.current_config.modules_mode != ModulesMode::AUTOMATIC) && !emuenv.cfg.current_config.lle_modules.empty()) {
        std::string modules;
        for (const auto &mod : emuenv.cfg.current_config.lle_modules) {
            modules += mod + ",";
        }
        modules.pop_back();
        LOG_INFO("lle-modules: {}", modules);
    }

    LOG_INFO("Title: {}", emuenv.current_app_title);
    LOG_INFO("Serial: {}", emuenv.io.title_id);
    LOG_INFO("Version: {}", emuenv.app_info.app_version);
    LOG_INFO("Category: {}", emuenv.app_info.app_category);

    init_device_paths(emuenv.io);
    init_savedata_app_path(emuenv.io, emuenv.vita_fs_path);

    // Load param.sfo
    vfs::FileBuffer param_sfo;
    if (vfs::read_app_file(param_sfo, emuenv.vita_fs_path, emuenv.io.app_path, "sce_sys/param.sfo"))
        sfo::load(emuenv.sfo_handle, param_sfo);

    init_exported_vars(emuenv);

    // Load main executable
    if (!launch_request.self_path.empty()) {
        emuenv.self_path = launch_request.self_path;
    } else {
        emuenv.self_path = !emuenv.cfg.self_path.empty() ? emuenv.cfg.self_path : EBOOT_PATH;
    }

    main_module_id = load_module(emuenv, "app0:" + emuenv.self_path);

    if (main_module_id >= 0) {
        const auto module = emuenv.kernel.loaded_modules[main_module_id];
        LOG_INFO("Main executable {} ({}) loaded", module->info.module_name, emuenv.self_path);
        const Patches patches = get_patches(emuenv.patch_path, emuenv.io.title_id, "app0:" + emuenv.self_path);
        if (!patches.empty())
            do_patches(emuenv.mem, patches, module->info);
    } else
        return FileNotFound;
    // Set self name from self path, can contain folder, get file name only
    emuenv.self_name = fs_utils::path_to_utf8(fs::path(emuenv.self_path).filename());

    // get list of preload modules
    SceUInt32 process_preload_disabled = 0;
    auto process_param = emuenv.kernel.process_param.get(emuenv.mem);
    if (process_param) {
        auto preload_disabled_ptr = Ptr<SceUInt32>(process_param->process_preload_disabled);
        if (preload_disabled_ptr) {
            process_preload_disabled = *preload_disabled_ptr.get(emuenv.mem);
        }
    }
    const auto module_app_path{ emuenv.vita_fs_path / "ux0/app" / emuenv.io.app_path / "sce_module" };

    std::vector<std::string> lib_load_list = {};
    // todo: check if module is imported
    auto add_preload_module = [&](uint32_t code, SceSysmoduleModuleId module_id, const std::string &name, bool load_from_app) {
        if ((process_preload_disabled & code) == 0) {
            if (is_lle_module(name, emuenv)) {
                const auto module_name_file = fmt::format("{}.suprx", name);
                if (load_from_app && fs::exists(module_app_path / module_name_file))
                    lib_load_list.emplace_back(fmt::format("app0:sce_module/{}", module_name_file));
                else if (fs::exists(emuenv.vita_fs_path / "vs0/sys/external" / module_name_file))
                    lib_load_list.emplace_back(fmt::format("vs0:sys/external/{}", module_name_file));
            }

            if (module_id != SCE_SYSMODULE_INVALID)
                emuenv.kernel.loaded_sysmodules[module_id] = {};
        }
    };
    lib_load_list.emplace_back("os0:kd/bootimage.skprx");
    lib_load_list.emplace_back("os0:kd/sysmodule.skprx");
    add_preload_module(0x00010000, SCE_SYSMODULE_INVALID, "libc", true);
    add_preload_module(0x00020000, SCE_SYSMODULE_DBG, "libdbg", false);
    add_preload_module(0x00080000, SCE_SYSMODULE_INVALID, "libshellsvc", false);
    add_preload_module(0x00100000, SCE_SYSMODULE_INVALID, "libcdlg", false);
    add_preload_module(0x00200000, SCE_SYSMODULE_FIOS2, "libfios2", true);
    add_preload_module(0x00400000, SCE_SYSMODULE_APPUTIL, "apputil", false);
    add_preload_module(0x00800000, SCE_SYSMODULE_INVALID, "libSceFt2", false);
    add_preload_module(0x01000000, SCE_SYSMODULE_INVALID, "libpvf", false);
    add_preload_module(0x02000000, SCE_SYSMODULE_PERF, "libperf", false); // if DEVELOPMENT_MODE dipsw is set

    for (const auto &module_path : lib_load_list) {
        auto res = load_module(emuenv, module_path);
        LOG_ERROR_IF(res < 0, "Failed to load preloaded module: {}. Ignoring this error.", module_path);
    }

    // Load taiHEN plugins configured for this title
    load_taihen_plugins_for_title(emuenv, emuenv.io.title_id);

    return Success;
}

void toggle_texture_replacement(EmuEnvState &emuenv) {
    emuenv.cfg.current_config.import_textures = !emuenv.cfg.current_config.import_textures;
    emuenv.renderer->get_texture_cache()->set_replacement_state(emuenv.cfg.current_config.import_textures, emuenv.cfg.current_config.export_textures, emuenv.cfg.current_config.export_as_png);
}

static std::vector<uint32_t> get_current_app_frame(EmuEnvState &emuenv, uint32_t &width, uint32_t &height) {
    // Dump the current frame from the emulator display
    std::vector<uint32_t> frame = emuenv.renderer->dump_frame(emuenv.display, width, height);
    if (frame.empty() || (frame.size() != (width * height))) {
        return {};
    }

    // Force alpha channel to 255 (fully opaque) for every pixel
    for (uint32_t &pixel : frame) {
        pixel |= 0xFF000000;
    }

    return frame;
}

void take_screenshot(EmuEnvState &emuenv) {
    if (emuenv.cfg.screenshot_format == None)
        return;

    if (emuenv.io.title_id.empty()) {
        LOG_ERROR("Trying to take a screenshot while not ingame");
        return;
    }

    uint32_t width, height;
    auto frame = get_current_app_frame(emuenv, width, height);
    if (frame.empty()) {
        LOG_ERROR("Failed to take screenshot");
        return;
    }

    const fs::path save_folder = emuenv.shared_path / "screenshots" / fmt::format("{}", string_utils::remove_special_chars(emuenv.current_app_title));
    fs::create_directories(save_folder);

    auto t = std::time(nullptr);
    struct tm localtime;
#ifdef _WIN32
    localtime_s(&localtime, &t);
#else
    localtime_r(&t, &localtime);
#endif

    const auto img_format = emuenv.cfg.screenshot_format == JPEG ? ".jpg" : ".png";
    const fs::path save_file = save_folder / fmt::format("{}_{:%Y-%m-%d-%H%M%OS}{}", string_utils::remove_special_chars(emuenv.current_app_title), localtime, img_format);
    constexpr int quality = 85; // google recommended value
    if (emuenv.cfg.screenshot_format == JPEG) {
        if (stbi_write_jpg(fs_utils::path_to_utf8(save_file).c_str(), width, height, 4, frame.data(), quality) == 1)
            LOG_INFO("Successfully saved screenshot to {}", save_file);
        else
            LOG_INFO("Failed to save screenshot");
    } else {
        if (stbi_write_png(fs_utils::path_to_utf8(save_file).c_str(), width, height, 4, frame.data(), width * 4) == 1)
            LOG_INFO("Successfully saved screenshot to {}", save_file);
        else
            LOG_INFO("Failed to save screenshot");
    }
}

ExitCode load_app(int32_t &main_module_id, EmuEnvState &emuenv) {
    return load_app(main_module_id, emuenv, AppLaunchRequest{
                                                .app_path = emuenv.io.app_path,
                                            });
}

ExitCode load_app(int32_t &main_module_id, EmuEnvState &emuenv, const AppLaunchRequest &launch_request) {
    if (load_app_impl(main_module_id, emuenv, launch_request) != Success) {
        std::string message = fmt::format(fmt::runtime(lang::get(lang::str::load_app_failed_msg)), emuenv.vita_fs_path / "ux0/app" / emuenv.io.app_path / emuenv.self_path);
        LOG_ERROR(message);
        return ModuleLoadFailed;
    }

    if (emuenv.cfg.gdbstub) {
        emuenv.kernel.debugger.wait_for_debugger = true;
        server_open(emuenv);
    }

#if USE_DISCORD
    if (emuenv.cfg.discord_rich_presence)
        discordrpc::update_presence(emuenv.io.title_id, emuenv.current_app_title);
#endif

    return Success;
}

static std::vector<std::string> split(const std::string &input, const std::string &regex) {
    std::regex re(regex);
    std::sregex_token_iterator
        first{ input.begin(), input.end(), re, -1 },
        last;
    return { first, last };
}

ExitCode run_app(EmuEnvState &emuenv, int32_t main_module_id) {
    return run_app(emuenv, main_module_id, AppLaunchRequest{
                                               .app_path = emuenv.io.app_path,
                                           });
}

ExitCode run_app(EmuEnvState &emuenv, int32_t main_module_id, const AppLaunchRequest &launch_request) {
    auto entry_point = emuenv.kernel.loaded_modules[main_module_id]->info.start_entry;
    auto process_param = emuenv.kernel.process_param.get(emuenv.mem);

    SceInt32 priority = SCE_KERNEL_DEFAULT_PRIORITY_USER;
    SceInt32 stack_size = SCE_KERNEL_STACK_SIZE_USER_MAIN;
    SceInt32 affinity = SCE_KERNEL_THREAD_CPU_AFFINITY_MASK_DEFAULT;
    if (process_param) {
        auto priority_ptr = Ptr<int32_t>(process_param->main_thread_priority);
        if (priority_ptr) {
            priority = *priority_ptr.get(emuenv.mem);
        }

        auto stack_size_ptr = Ptr<int32_t>(process_param->main_thread_stacksize);
        if (stack_size_ptr) {
            stack_size = *stack_size_ptr.get(emuenv.mem);
        }

        auto affinity_ptr = Ptr<SceInt32>(process_param->main_thread_cpu_affinity_mask);
        if (affinity_ptr) {
            affinity = *affinity_ptr.get(emuenv.mem);
        }
    }
    const ThreadStatePtr main_thread = emuenv.kernel.create_thread(emuenv.mem, emuenv.io.title_id.c_str(), entry_point, priority, affinity, stack_size, nullptr);
    if (!main_thread) {
        LOG_ERROR("Failed to init main thread.");
        return InitThreadFailed;
    }
    emuenv.main_thread_id = main_thread->id;

    // Run `module_start` export (entry point) of loaded libraries
    for (auto &[_, module] : emuenv.kernel.loaded_modules) {
        if (module->info.modid != main_module_id)
            start_module(emuenv, module->info);
    }

    SceKernelThreadOptParam param{ 0, 0 };
    std::vector<std::string> cfg_args;
    const auto *args = &launch_request.argv;
    if (args->empty() && !emuenv.cfg.app_args.empty()) {
        cfg_args = split(emuenv.cfg.app_args, ",\\s+");
        args = &cfg_args;
    }
    if (!args->empty()) {
        // why is this flipped
        std::vector<uint8_t> buf;
        for (const auto &arg : *args)
            buf.insert(buf.end(), arg.c_str(), arg.c_str() + arg.size() + 1);
        auto arr = Ptr<uint8_t>(alloc(emuenv.mem, static_cast<uint32_t>(buf.size()), "arg"));
        memcpy(arr.get(emuenv.mem), buf.data(), buf.size());
        param.size = static_cast<SceSize>(buf.size());
        param.attr = arr.address();
    }
    if (main_thread->start(param.size, Ptr<void>(param.attr), true) < 0) {
        LOG_ERROR("Failed to run main thread.");
        return RunThreadFailed;
    }

    start_sync_thread(emuenv);

    return Success;
}
