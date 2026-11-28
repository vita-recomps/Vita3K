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

import java.io.File;
import java.io.IOException;
import java.io.RandomAccessFile;
import java.nio.charset.StandardCharsets;
import java.util.regex.Pattern;

/** Reads an existing log only when a report is requested. Does not change the source file. */
final class DiagnosticLog {
    private static final Pattern KEY_LINE =
            Pattern.compile(
                    ".*(zrif|license.?key|licence.?key|rif.?key).*",
                    Pattern.CASE_INSENSITIVE | Pattern.DOTALL);

    static String tail(File file, int limit) throws IOException {
        if (limit <= 0) throw new IllegalArgumentException("limit must be positive");
        try (RandomAccessFile in = new RandomAccessFile(file, "r")) {
            long end = in.length();
            long start = Math.max(0, end - limit);
            boolean partial = false;
            if (start > 0) {
                in.seek(start - 1);
                partial = in.read() != '\n';
            }
            in.seek(start);
            byte[] bytes = new byte[(int) (end - start)];
            in.readFully(bytes);
            String text = new String(bytes, StandardCharsets.UTF_8);
            if (partial) {
                int newline = text.indexOf('\n');
                text = newline >= 0 ? text.substring(newline + 1) : "";
            }
            StringBuilder redacted = new StringBuilder();
            for (String line : text.split("\n")) {
                redacted.append(KEY_LINE.matcher(line).matches() ? "[key line omitted]" : line)
                        .append('\n');
            }
            return redacted.toString();
        }
    }
}
