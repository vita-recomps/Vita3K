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

import android.app.*;
import android.content.*;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.widget.Toast;

import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;
import java.security.MessageDigest;
import java.util.*;
import java.util.zip.*;

public final class DriverImport {
    public static final int REQUEST = 4761;

    public static void pick(Activity a) {
        a.startActivity(new Intent(a, DriverPickerActivity.class));
    }

    public static void result(Activity a, int request, int code, Intent data) {
        if (request != REQUEST
                || code != Activity.RESULT_OK
                || data == null
                || data.getData() == null) return;
        Uri uri = data.getData();
        Toast.makeText(a, "Importando driver...", Toast.LENGTH_SHORT).show();
        new Thread(
                        () -> {
                            try {
                                String name = "driver";
                                try (Cursor c =
                                        a.getContentResolver()
                                                .query(
                                                        uri,
                                                        new String[] {OpenableColumns.DISPLAY_NAME},
                                                        null,
                                                        null,
                                                        null)) {
                                    if (c != null && c.moveToFirst()) name = c.getString(0);
                                }
                                String installed = install(a, uri, name);
                                ImportDiagnostics.note(
                                        a,
                                        "Driver importado: "
                                                + installed
                                                + "; aplicacao somente na proxima inicializacao.");
                                a.runOnUiThread(
                                        () ->
                                                new AlertDialog.Builder(a)
                                                        .setTitle("Driver salvo")
                                                        .setMessage(
                                                                "Salvo como "
                                                                        + installed
                                                                        + ".\n\n"
                                                                        + "Feche e reabra o"
                                                                        + " aplicativo. Em Debug >"
                                                                        + " GPU driver, escolha"
                                                                        + " esse nome (ou Auto para"
                                                                        + " o mais recente). A"
                                                                        + " escolha sera aplicada"
                                                                        + " na inicializacao"
                                                                        + " seguinte. Turnip e"
                                                                        + " destinado a GPUs"
                                                                        + " Adreno, nao ao PowerVR"
                                                                        + " do Moto G54.")
                                                        .setPositiveButton(
                                                                "OK",
                                                                (dialog, which) -> {
                                                                    if (a
                                                                            instanceof
                                                                            DriverPickerActivity)
                                                                        a.finish();
                                                                })
                                                        .show());
                            } catch (Exception e) {
                                ImportDiagnostics.note(a, "Importacao de driver falhou: " + e);
                                a.runOnUiThread(
                                        () ->
                                                new AlertDialog.Builder(a)
                                                        .setTitle("Driver nao importado")
                                                        .setMessage(e.getMessage())
                                                        .setPositiveButton(
                                                                "OK",
                                                                (dialog, which) -> {
                                                                    if (a
                                                                            instanceof
                                                                            DriverPickerActivity)
                                                                        a.finish();
                                                                })
                                                        .show());
                            }
                        },
                        "driver-import")
                .start();
    }

    private static String safe(String path) throws IOException {
        if (path.length() > 240
                || path.startsWith("/")
                || path.contains("\\")
                || path.contains(":")) throw new IOException("Caminho invalido no ZIP");
        for (String part : path.split("/"))
            if (part.equals("..") || part.equals("."))
                throw new IOException("Caminho inseguro no ZIP");
        return path;
    }

    static String install(Activity a, Uri uri, String display) throws Exception {
        File incoming = File.createTempFile("driver-", ".zip", a.getCacheDir());
        File normalized = null;
        try {
            MessageDigest digest = MessageDigest.getInstance("SHA-256");
            long total = 0;
            try (InputStream in = a.getContentResolver().openInputStream(uri);
                    OutputStream out = new FileOutputStream(incoming)) {
                if (in == null) throw new IOException("Sem acesso ao ZIP");
                byte[] b = new byte[65536];
                int n;
                while ((n = in.read(b)) != -1) {
                    total += n;
                    if (total > 134217728) throw new IOException("ZIP excede 128 MiB");
                    digest.update(b, 0, n);
                    out.write(b, 0, n);
                }
            }
            List<String> candidates = new ArrayList<>();
            boolean marker = false;
            long expanded = 0;
            Set<String> names = new HashSet<>();
            try (ZipFile z = new ZipFile(incoming)) {
                Enumeration<? extends ZipEntry> entries = z.entries();
                while (entries.hasMoreElements()) {
                    ZipEntry e = entries.nextElement();
                    String n = safe(e.getName());
                    if (!names.add(n) || names.size() > 2048)
                        throw new IOException("ZIP duplicado ou excessivo");
                    if (e.isDirectory()) continue;
                    if (e.getSize() < 0 || (expanded += e.getSize()) > 536870912)
                        throw new IOException("Conteudo excede 512 MiB");
                    if (n.equals("meta.json") || n.equals("driver_name.txt")) marker = true;
                    if (n.toLowerCase(Locale.ROOT).contains("vulkan") && n.endsWith(".so")) {
                        try (InputStream s = z.getInputStream(e)) {
                            byte[] h = new byte[20];
                            int at = 0, r;
                            while (at < h.length && (r = s.read(h, at, h.length - at)) > 0) at += r;
                            if (at != 20
                                    || h[0] != 127
                                    || h[1] != 69
                                    || h[2] != 76
                                    || h[3] != 70
                                    || h[4] != 2
                                    || h[5] != 1
                                    || (h[18] & 255) != 183
                                    || h[19] != 0)
                                throw new IOException("Biblioteca Vulkan nao e ARM64");
                        }
                        candidates.add(n);
                    }
                }
                if (candidates.isEmpty())
                    throw new IOException("Nenhuma biblioteca Vulkan ARM64 encontrada no ZIP");
                if (!marker && candidates.size() != 1)
                    throw new IOException(
                            "Mais de uma biblioteca Vulkan; nao foi possivel identificar a"
                                + " principal");
                normalized = File.createTempFile("driver-normalized-", ".zip", a.getCacheDir());
                try (ZipOutputStream out = new ZipOutputStream(new FileOutputStream(normalized))) {
                    for (String n : names) {
                        ZipEntry e = z.getEntry(n);
                        out.putNextEntry(new ZipEntry(n));
                        if (!e.isDirectory())
                            try (InputStream s = z.getInputStream(e)) {
                                byte[] b = new byte[65536];
                                int r;
                                long copied = 0;
                                while ((r = s.read(b)) != -1) {
                                    copied += r;
                                    if (copied > 536870912)
                                        throw new IOException("Entrada excessiva");
                                    out.write(b, 0, r);
                                }
                            }
                        out.closeEntry();
                    }
                    if (!marker) {
                        out.putNextEntry(new ZipEntry("driver_name.txt"));
                        out.write(candidates.get(0).getBytes(StandardCharsets.UTF_8));
                        out.closeEntry();
                    }
                }
            }
            StringBuilder hash = new StringBuilder();
            for (byte b : digest.digest()) hash.append(String.format(Locale.ROOT, "%02x", b & 255));
            String stem =
                    (display == null ? "driver" : display)
                            .replaceFirst("(?i)(\\.adpkg)?\\.zip$", "")
                            .replaceAll("[^A-Za-z0-9._-]", "_");
            if (stem.length() > 70) stem = stem.substring(0, 70);
            String id = stem + "-" + hash.substring(0, 8);
            File dir = new File(ImportDiagnostics.base(a), "drivers");
            if (!dir.isDirectory() && !dir.mkdirs())
                throw new IOException("Sem acesso a pasta de drivers");
            Files.move(
                    normalized.toPath(),
                    new File(dir, id + ".zip").toPath(),
                    StandardCopyOption.REPLACE_EXISTING);
            return id;
        } finally {
            incoming.delete();
            if (normalized != null) normalized.delete();
        }
    }
}
