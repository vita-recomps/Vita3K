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

// The game's add-ons (costumes, pacts) ship in its own data since the last patch; PSN only
// recorded which were bought. This checks that the installed game has them.
#pragma once

#include <util/fs.h>

#include <string>

namespace ss_delta {

// True when the installed game (app_dir = ux0/app/PCSA00152) has the add-ons' pacts and raiments
// in its data; otherwise `why` says what is missing.
bool addon_data_present(const fs::path &app_dir, std::string &why);

} // namespace ss_delta
