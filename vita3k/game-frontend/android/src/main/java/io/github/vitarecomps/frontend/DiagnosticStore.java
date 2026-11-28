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

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;

/** App-private, write-once snapshots. No report is replaced or automatically removed. */
final class DiagnosticStore {
    static File create(File directory, String body) throws IOException {
        if (!directory.isDirectory() && !directory.mkdirs())
            throw new IOException("Cannot create report directory");
        File file;
        do {
            file = new File(directory, "report-" + UUID.randomUUID() + ".txt");
        } while (!file.createNewFile());
        boolean complete = false;
        try {
            try (Writer out =
                    new OutputStreamWriter(new FileOutputStream(file), StandardCharsets.UTF_8)) {
                out.write(body);
            }
            complete = true;
            return file;
        } finally {
            if (!complete) file.delete();
        }
    }

    static File resolve(File directory, String name) throws IOException {
        if (name == null
                || !name.matches(
                        "report-[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}\\.txt"))
            throw new FileNotFoundException("Invalid report name");
        File root = directory.getCanonicalFile();
        File file = new File(root, name).getCanonicalFile();
        if (!root.equals(file.getParentFile()) || !file.isFile())
            throw new FileNotFoundException("Report unavailable");
        return file;
    }

    static String[] columns(String[] requested) {
        if (requested == null) return new String[] {"_display_name", "_size"};
        List<String> supported = new ArrayList<>();
        for (String name : requested)
            if ("_display_name".equals(name) || "_size".equals(name)) supported.add(name);
        return supported.toArray(new String[0]);
    }
}
