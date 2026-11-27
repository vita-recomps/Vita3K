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
import java.util.zip.*;

public class ArmModuleFilesTest {
    static int checks;

    static void expectFailure(RunnableIO action) throws Exception {
        try {
            action.run();
            throw new AssertionError("Expected rejection");
        } catch (IOException expected) {
            checks++;
        }
    }

    interface RunnableIO {
        void run() throws Exception;
    }

    static byte[] elf() {
        byte[] b = new byte[64];
        b[0] = 127;
        b[1] = 'E';
        b[2] = 'L';
        b[3] = 'F';
        b[4] = 2;
        b[5] = 1;
        b[6] = 1;
        b[16] = 3;
        b[18] = (byte) 183;
        return b;
    }

    static void fill(File folder) throws Exception {
        for (String name : ArmModuleFiles.NAMES)
            Files.write(new File(folder, name).toPath(), elf());
    }

    static File zip(File root, String name, String entry) throws Exception {
        File out = new File(root, name);
        try (ZipOutputStream z = new ZipOutputStream(new FileOutputStream(out))) {
            z.putNextEntry(new ZipEntry(entry));
            z.write(elf());
            z.closeEntry();
        }
        return out;
    }

    public static void main(String[] args) throws Exception {
        File root = Files.createTempDirectory("arm-import-tests").toFile(),
                stage = new File(root, "stage");
        stage.mkdir();
        try {
            expectFailure(() -> ArmModuleFiles.manifest(stage));
            fill(stage);
            String manifest = ArmModuleFiles.manifest(stage);
            if (!manifest.contains("ssd_aot_eboot.so") || !manifest.contains("sha256"))
                throw new AssertionError();
            checks++;
            File out = new File(root, "complete.zip");
            ArmModuleFiles.pack(stage, out);
            File unpack = new File(root, "unpack");
            unpack.mkdir();
            ArmModuleFiles.unpack(out, unpack);
            if (!manifest.equals(ArmModuleFiles.manifest(unpack))) throw new AssertionError();
            checks++;
            byte[] bad = elf();
            bad[18] = 62;
            Files.write(new File(stage, ArmModuleFiles.NAMES[0]).toPath(), bad);
            expectFailure(() -> ArmModuleFiles.manifest(stage));
            bad = elf();
            bad[16] = 2;
            Files.write(new File(stage, ArmModuleFiles.NAMES[0]).toPath(), bad);
            expectFailure(() -> ArmModuleFiles.manifest(stage));
            File hostile = zip(root, "traversal.zip", "../ssd_aot_eboot.so");
            expectFailure(() -> ArmModuleFiles.unpack(hostile, stage));
            File unknown = zip(root, "unknown.zip", "runtime.so");
            expectFailure(() -> ArmModuleFiles.unpack(unknown, stage));
            File wrapped = new File(root, "wrapped.zip");
            try (ZipOutputStream z = new ZipOutputStream(new FileOutputStream(wrapped))) {
                for (String name : ArmModuleFiles.NAMES) {
                    z.putNextEntry(new ZipEntry("modules/" + name));
                    z.write(elf());
                    z.closeEntry();
                }
            }
            ArmModuleFiles.unpack(wrapped, stage);
            ArmModuleFiles.manifest(stage);
            checks++;
            File duplicates = new File(root, "duplicates.zip");
            try (ZipOutputStream z = new ZipOutputStream(new FileOutputStream(duplicates))) {
                for (String entry : new String[] {"ssd_aot_eboot.so", "folder/ssd_aot_eboot.so"}) {
                    z.putNextEntry(new ZipEntry(entry));
                    z.write(elf());
                    z.closeEntry();
                }
            }
            expectFailure(() -> ArmModuleFiles.unpack(duplicates, stage));
            expectFailure(
                    () ->
                            ArmModuleFiles.copy(
                                    new ByteArrayInputStream(new byte[65]),
                                    new File(root, "limit"),
                                    64));
            System.out.println(
                    "PASS: "
                            + checks
                            + " checks (complete set, auto manifest, round trip, wrong arch/type,"
                            + " ZIP paths/duplicates, wrapped folder, bounded copies)");
        } finally {
            delete(root);
        }
    }

    static void delete(File f) {
        File[] children = f.listFiles();
        if (children != null) for (File c : children) delete(c);
        f.delete();
    }
}
