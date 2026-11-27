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

import android.app.Activity;
import android.content.ClipData;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.net.Uri;
import android.os.Build;
import android.util.Log;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

/** User-requested snapshots of existing logs; no periodic tracing or performance counters. */
public final class ImportDiagnostics {
    private static final int LOG_TAIL_BYTES = 192 * 1024;
    private static final int ACCESS_LOG_BYTES = 512 * 1024;

    private ImportDiagnostics() {}

    static File base(Context context) {
        File external = context.getExternalFilesDir(null);
        return external != null ? external : context.getFilesDir();
    }

    static File reportFile(Context context) {
        return new File(base(context), "import-diagnostic.txt");
    }

    public static synchronized void note(Context context, String message) {
        File file = new File(base(context), "import-access.log");
        try {
            if (file.length() > ACCESS_LOG_BYTES) new FileOutputStream(file).close();
            try (Writer out =
                    new OutputStreamWriter(
                            new FileOutputStream(file, true), StandardCharsets.UTF_8)) {
                out.write(new Date() + " " + message + "\n");
            }
        } catch (IOException error) {
            Log.e("ImportDiagnostics", "Cannot write diagnostic", error);
        }
    }

    private static String gameLibrary(Context context) throws PackageManager.NameNotFoundException {
        ActivityInfo info =
                context.getPackageManager()
                        .getActivityInfo(
                                new ComponentName(context, GameActivity.class),
                                PackageManager.GET_META_DATA);
        String library = info.metaData != null ? info.metaData.getString("game.library") : null;
        return library != null && library.matches("[A-Za-z0-9._-]+") ? library : "main";
    }

    private static void append(StringBuilder text, File file) {
        if (!file.isFile()) return;
        text.append("\n--- ").append(file.getName()).append(" ---\n");
        try {
            text.append(DiagnosticLog.tail(file, LOG_TAIL_BYTES));
        } catch (IOException error) {
            text.append("Cannot read log: ").append(error).append('\n');
        }
    }

    private static synchronized File snapshot(Context context)
            throws IOException, PackageManager.NameNotFoundException {
        StringBuilder text =
                new StringBuilder(
                                context.getApplicationInfo().loadLabel(context.getPackageManager()))
                        .append(" - diagnostic report\n");
        text.append("Package: ").append(context.getPackageName()).append('\n');
        text.append("Version: ")
                .append(
                        context.getPackageManager()
                                .getPackageInfo(context.getPackageName(), 0)
                                .versionName)
                .append('\n');
        text.append("Device: ")
                .append(Build.MANUFACTURER)
                .append(' ')
                .append(Build.MODEL)
                .append('\n');
        text.append("Android: ")
                .append(Build.VERSION.RELEASE)
                .append("\nExported: ")
                .append(new Date())
                .append('\n');
        File root = base(context);
        append(text, new File(root, "import-access.log"));
        append(text, new File(root, "setup.log"));
        append(text, new File(new File(root, gameLibrary(context)), "setup.log"));
        append(text, new File(new File(root, gameLibrary(context)), "vita3k.log"));
        File report = reportFile(context);
        try (Writer out =
                new OutputStreamWriter(new FileOutputStream(report), StandardCharsets.UTF_8)) {
            out.write(text.toString());
        }
        return report;
    }

    public static void show(Activity activity) {
        try {
            File file = snapshot(activity);
            String body =
                    new String(
                            java.nio.file.Files.readAllBytes(file.toPath()),
                            StandardCharsets.UTF_8);
            if (body.length() > 45000)
                body =
                        body.substring(0, 3000)
                                + "\n[display shortened; sharing keeps the full report]\n"
                                + body.substring(body.length() - 42000);
            android.widget.TextView text = new android.widget.TextView(activity);
            text.setText(body);
            text.setTextSize(12);
            text.setTextIsSelectable(true);
            int padding = Math.round(12 * activity.getResources().getDisplayMetrics().density);
            text.setPadding(padding, padding, padding, padding);
            android.widget.ScrollView scroll = new android.widget.ScrollView(activity);
            scroll.addView(text);
            new android.app.AlertDialog.Builder(activity)
                    .setTitle("Diagnostic report")
                    .setView(scroll)
                    .setPositiveButton("Close", null)
                    .setNeutralButton("Share", (dialog, which) -> share(activity))
                    .show();
        } catch (IOException | PackageManager.NameNotFoundException error) {
            Log.e("ImportDiagnostics", "Cannot view report", error);
            Toast.makeText(activity, "Cannot create the diagnostic report", Toast.LENGTH_LONG)
                    .show();
        }
    }

    public static void share(Activity activity) {
        try {
            snapshot(activity);
            String stamp =
                    new SimpleDateFormat("yyyy-MM-dd_HH-mm-ss", Locale.ROOT).format(new Date());
            String name =
                    "SSD_" + Build.MODEL.replaceAll("[^A-Za-z0-9_-]", "_") + "_" + stamp + ".txt";
            Uri uri = Uri.parse("content://" + activity.getPackageName() + ".importdiag/" + name);
            Intent intent = new Intent(Intent.ACTION_SEND).setType("text/plain");
            intent.putExtra(Intent.EXTRA_STREAM, uri);
            intent.putExtra(Intent.EXTRA_SUBJECT, name);
            intent.setClipData(ClipData.newRawUri("Diagnostic report", uri));
            intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            activity.startActivity(Intent.createChooser(intent, "Share diagnostic report"));
        } catch (IOException | PackageManager.NameNotFoundException error) {
            Log.e("ImportDiagnostics", "Cannot create report", error);
            Toast.makeText(activity, "Cannot create the diagnostic report", Toast.LENGTH_LONG)
                    .show();
        }
    }
}
