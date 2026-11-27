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
import java.nio.file.*;

public class InstallProgressLogTest {
    static void require(boolean ok) {
        if (!ok) throw new AssertionError();
    }

    public static void main(String[] args) throws Exception {
        File f = File.createTempFile("progress", ".log");
        try {
            Files.write(f.toPath(), "old Installation: 100%\n".getBytes());
            InstallProgressLog r = new InstallProgressLog(f, 1);
            r.poll(1);
            require(r.percent == -1);
            Files.write(
                    f.toPath(),
                    "Installing picked-0.pkg\nInstallation: 20%\n".getBytes(),
                    StandardOpenOption.APPEND);
            r.poll(1);
            require(r.percent == 20);
            Files.write(f.toPath(), "Installation: 30".getBytes(), StandardOpenOption.APPEND);
            r.poll(1);
            require(r.percent == 20);
            Files.write(f.toPath(), "%\n".getBytes(), StandardOpenOption.APPEND);
            r.poll(1);
            require(r.percent == 30);
            Files.write(
                    f.toPath(), "Installing firmware x\nFirmware installation: 7%\n".getBytes());
            r.poll(2);
            require(r.percent == 7);
            r.accept("Installation: 999%\n");
            require(r.percent == 7);
            r.accept("Installing firmware next\n");
            require(r.percent == -1);
            System.out.println(
                    "PASS: 6 progress checks (old log ignored, appended %, partial line, rotation,"
                        + " invalid %, next PUP reset)");
        } finally {
            f.delete();
        }
    }
}
