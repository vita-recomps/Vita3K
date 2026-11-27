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

package io.github.vitarecomps.frontend;

import android.content.Context;

import org.json.JSONObject;

import java.io.*;

final class ImportedModuleSet {
    static boolean reusable(Context context, JSONObject record) {
        if (!"User-imported ARM64".equals(record.optString("source"))) return false;
        JSONObject modules = record.optJSONObject("modules");
        if (modules == null) return false;
        File root = BuildActivity.gameDir(context);
        try {
            for (String name : ArmModuleFiles.NAMES) {
                JSONObject entry = modules.optJSONObject(name);
                File file = new File(root, name);
                if (entry == null
                        || !file.isFile()
                        || file.length() < 64
                        || file.length() != entry.optLong("bytes", -1)
                        || !entry.optString("sha256").matches("[0-9a-fA-F]{64}")) return false;
                try (RandomAccessFile in = new RandomAccessFile(file, "r")) {
                    byte[] h = new byte[20];
                    in.readFully(h);
                    if (h[0] != 127
                            || h[1] != 69
                            || h[2] != 76
                            || h[3] != 70
                            || h[4] != 2
                            || h[5] != 1
                            || h[16] != 3
                            || h[17] != 0
                            || (h[18] & 255) != 183
                            || h[19] != 0) return false;
                }
            }
            return true;
        } catch (IOException error) {
            return false;
        }
    }
}
