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
import java.util.regex.*;

/** Reads only newly appended setup log data; never synthesizes a percentage. */
public final class InstallProgressLog {
    private final File file;
    private long position, identity;
    private String partial = "";
    public int percent = -1;
    public String detail = "Preparing files...";
    private static final Pattern PROGRESS =
            Pattern.compile("(?:Firmware installation|Installation):\\s*(\\d{1,3})%");

    public InstallProgressLog(File file, long identity) {
        this.file = file;
        this.identity = identity;
        position = file.length();
    }

    public void poll(long currentIdentity) throws IOException {
        if (!file.isFile()) return;
        try (RandomAccessFile in = new RandomAccessFile(file, "r")) {
            if (currentIdentity != identity || in.length() < position) {
                position = 0;
                partial = "";
                identity = currentIdentity;
                percent = -1;
            }
            if (in.length() - position > 65536) {
                position = in.length() - 65536;
                partial = "";
            }
            in.seek(position);
            byte[] bytes = new byte[(int) (in.length() - position)];
            in.readFully(bytes);
            position += bytes.length;
            String text = partial + new String(bytes, StandardCharsets.UTF_8);
            int end = text.lastIndexOf('\n');
            if (end >= 0) {
                accept(text.substring(0, end));
                partial = text.substring(end + 1);
            } else partial = text;
        }
    }

    public void accept(String text) {
        for (String line : text.split("\\r?\\n")) {
            if (line.contains("Installing firmware ")) {
                percent = -1;
                detail = "Installing firmware / fonts...";
            } else if (line.contains("Installing ")) {
                percent = -1;
                detail = "Extracting and installing the PKG...";
            }
            Matcher matcher = PROGRESS.matcher(line);
            if (matcher.find()) {
                int n = Integer.parseInt(matcher.group(1));
                if (n >= 0 && n <= 100) {
                    percent = n;
                    detail = "Installation progress: " + n + "%";
                }
            }
        }
    }
}
