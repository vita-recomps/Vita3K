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

// The game's save on the PC: ux0:user/00/savedata/PCSA00152/ in the Vita file system holds
// datag0001.bin and system.bin, each with a .bak copy, unencrypted, plus Vita3K's record of the
// game's save slot 1 (SlotParam_1.bin). A save decrypted on a Vita (VitaShell, Apollo) has the
// same four files.
#pragma once

#include <util/fs.h>

#include <string>

namespace ss_delta {

// The game has three save slots. Copies a save into slot `slot` (1-3) from a folder or a .zip that
// holds a datag<n>.bin (in any subfolder; .bak copies and system.bin optional). The save folder as
// it was goes to `backup_dir`/<date and time>/ first. system.bin, shared by the slots, is kept if
// there is one. Returns false with the reason in `message`.
bool import_save(const fs::path &vita_fs, const fs::path &source, int slot, const fs::path &backup_dir, std::string &message);

// Copies the save in slot `slot` to the folder `dest`, as datag0001 plus system.bin.
bool export_save(const fs::path &vita_fs, const fs::path &dest, int slot, std::string &message);

// The add-ons (docs/decisions.md D14 in ss-delta): system.bin records what was bought on PSN as
// lists of ids. Adds the released add-ons to those lists in the save's system.bin and .bak,
// keeping what is there; the previous files go to `backup_dir`/<date and time>/ first. Does
// nothing when there is no save yet or the add-ons are recorded already. Returns false with the
// reason in `message` on an error; `message` also says what was done.
bool unlock_addons(const fs::path &vita_fs, const fs::path &backup_dir, std::string &message);

} // namespace ss_delta
