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
import java.security.*;
import java.util.*;
import java.util.zip.*;

/** Packages already compiled libraries. Does not compile or execute imported code. */
public final class ArmModuleFiles {
    public static final String[] NAMES = {
        "ssd_aot_eboot.so",
        "ssd_aot_libc.so",
        "ssd_aot_libfios2.so",
        "ssd_aot_libult.so",
        "ssd_aot_fw_sysmodule.so",
        "ssd_aot_fw_libSceFt2.so",
        "ssd_aot_fw_libpvf.so",
        "ssd_aot_fw_libscemp4.so",
        "ssd_aot_fw_libsceavplayer.so",
        "ssd_aot_fw_adhoc_matching.so"
    };
    public static final long MAX_FILE = 256L << 20, MAX_TOTAL = 512L << 20;

    public static boolean expected(String name) {
        return Arrays.asList(NAMES).contains(name);
    }

    public static String hex(byte[] bytes) {
        StringBuilder out = new StringBuilder();
        for (byte b : bytes) out.append(String.format(Locale.ROOT, "%02x", b & 255));
        return out.toString();
    }

    public static long copy(InputStream in, File file, long limit) throws IOException {
        long size = 0;
        byte[] buffer = new byte[65536];
        try (FileOutputStream out = new FileOutputStream(file)) {
            for (int n; (n = in.read(buffer)) != -1; ) {
                size += n;
                if (size > limit) throw new IOException("File exceeds the supported size");
                out.write(buffer, 0, n);
            }
            out.getFD().sync();
        }
        return size;
    }

    public static void unpack(File zipFile, File stage) throws IOException {
        Set<String> seen = new HashSet<>();
        long total = 0;
        int entries = 0;
        try (ZipInputStream zip =
                new ZipInputStream(new BufferedInputStream(new FileInputStream(zipFile)))) {
            for (ZipEntry entry; (entry = zip.getNextEntry()) != null; ) {
                if (++entries > 32)
                    throw new IOException(
                            "Too many files in the ZIP; select only the ARM64 modules");
                String path = entry.getName();
                if (path.startsWith("/")
                        || path.contains("\\")
                        || Arrays.asList(path.split("/", -1)).contains(".."))
                    throw new IOException("Unsafe ZIP path");
                if (entry.isDirectory()) continue;
                String[] parts = path.split("/", -1);
                if (parts.length > 2)
                    throw new IOException(
                            "Put the modules together, with at most one enclosing folder");
                String name = parts[parts.length - 1];
                if (!seen.add(name)) throw new IOException("Duplicate file: " + name);
                if (name.equals("modules.json") || name.equals("modulos-arm64.json")) {
                    total += copy(zip, new File(stage, "source-manifest.json"), 65536);
                    continue;
                }
                if (!expected(name))
                    throw new IOException(
                            "Unexpected file: " + name + ". Select only the compiled modules.");
                total += copy(zip, new File(stage, name), Math.min(MAX_FILE, MAX_TOTAL - total));
                if (total > MAX_TOTAL) throw new IOException("Module package too large");
            }
        }
    }

    public static String manifest(File folder) throws IOException {
        StringBuilder json = new StringBuilder("{\n");
        long total = 0;
        for (int i = 0; i < NAMES.length; i++) {
            String name = NAMES[i];
            File file = new File(folder, name);
            if (!file.isFile()) throw new IOException("Missing module: " + name);
            long size = file.length();
            total += size;
            if (size < 64 || size > MAX_FILE || total > MAX_TOTAL)
                throw new IOException("Invalid module size: " + name);
            byte[] header = new byte[64];
            try (DataInputStream in = new DataInputStream(new FileInputStream(file))) {
                in.readFully(header);
            }
            if (header[0] != 127
                    || header[1] != 'E'
                    || header[2] != 'L'
                    || header[3] != 'F'
                    || header[4] != 2
                    || header[5] != 1
                    || header[6] != 1
                    || header[16] != 3
                    || header[17] != 0
                    || (header[18] & 255) != 183
                    || header[19] != 0)
                throw new IOException("Not an ARM64 shared library: " + name);
            MessageDigest hash;
            try {
                hash = MessageDigest.getInstance("SHA-256");
            } catch (NoSuchAlgorithmException e) {
                throw new AssertionError(e);
            }
            try (InputStream in = new FileInputStream(file)) {
                byte[] buffer = new byte[65536];
                for (int n; (n = in.read(buffer)) != -1; ) hash.update(buffer, 0, n);
            }
            if (i > 0) json.append(",\n");
            json.append("  \"")
                    .append(name)
                    .append("\": {\"sha256\": \"")
                    .append(hex(hash.digest()))
                    .append("\", \"bytes\": ")
                    .append(size)
                    .append('}');
        }
        return json.append("\n}\n").toString();
    }

    public static void pack(File stage, File output) throws IOException {
        String manifest = manifest(stage);
        try (ZipOutputStream zip =
                new ZipOutputStream(new BufferedOutputStream(new FileOutputStream(output)))) {
            zip.putNextEntry(new ZipEntry("modules.json"));
            zip.write(manifest.getBytes(java.nio.charset.StandardCharsets.UTF_8));
            zip.closeEntry();
            byte[] buffer = new byte[65536];
            for (String name : NAMES) {
                zip.putNextEntry(new ZipEntry(name));
                try (InputStream in = new FileInputStream(new File(stage, name))) {
                    for (int n; (n = in.read(buffer)) != -1; ) zip.write(buffer, 0, n);
                }
                zip.closeEntry();
            }
        }
    }

    public static void removeStage(File folder) throws IOException {
        File[] files = folder.listFiles();
        if (files != null)
            for (File file : files) {
                if (!file.isFile() || !file.delete())
                    throw new IOException("Could not clear staging file: " + file);
            }
        if (folder.exists() && !folder.delete())
            throw new IOException("Could not clear staging folder");
    }
}
