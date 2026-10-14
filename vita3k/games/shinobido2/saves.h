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

// The game's saves on the PC: ux0:user/00/savedata/PCSE00015/ in the Vita file system holds one
// data<NNNN>.bin per save slot (slots 0-9, unencrypted), and Vita3K's record of each slot
// (SlotParam_<N>.bin: the title, play time and day the Load dialog shows). A save decrypted on a
// Vita (VitaShell, Apollo) has the data files; the slot records are Vita3K's own.
#pragma once

#include <util/fs.h>

#include <string>

namespace shinobido2 {

// Copies saves in from a folder or a .zip holding data<NNNN>.bin files (in any subfolder;
// SlotParam_<N>.bin optional). slot -1: each data file into its own slot; slot 0-9: the one data
// file there is (or data<slot> among several) into that slot. The save folder as it was goes to
// `backup_dir`/<date and time>/ first. A slot without a record gets one ("Imported"; the game
// writes its own at the next save). Returns false with the reason in `message`.
bool import_save(const fs::path &vita_fs, const fs::path &source, int slot, const fs::path &backup_dir, std::string &message);

// Copies the save in slot `slot` (or every slot, -1) to the folder `dest`, with its record.
bool export_save(const fs::path &vita_fs, const fs::path &dest, int slot, std::string &message);

} // namespace shinobido2
