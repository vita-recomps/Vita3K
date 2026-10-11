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

// The game's add-on content: unlock keys (PSN entitlements) for costumes, hoods and a sigil whose
// items ship in the game's own data. The list of keys is the game's
// resource/database/network/entitlement_table_us.csv, read from its archives.
#pragma once

#include <util/fs.h>

#include <string>
#include <vector>

namespace ss_delta {

struct DlcList {
    std::vector<std::string> ids; // product ids the game checks, e.g. UP9000-PCSA00152_00-SSC001
    std::vector<std::string> left_out; // rows of the table that are not released add-ons
    std::string source; // the archive the table came from; empty when there is none
};

// The released add-ons listed in the installed game's data (app_dir = ux0/app/PCSA00152).
DlcList find_dlc(const fs::path &app_dir);

} // namespace ss_delta
