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
import java.lang.ref.WeakReference;
import java.nio.charset.StandardCharsets;
import java.util.Date;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

/** User-requested snapshots of existing logs; no periodic tracing or performance counters. */
public final class ImportDiagnostics {
    private static final int LOG_TAIL_BYTES = 192 * 1024;
    private static final int ACCESS_LOG_BYTES = 512 * 1024;

    private static final ExecutorService REPORTS =
            Executors.newSingleThreadExecutor(
                    runnable -> {
                        Thread thread = new Thread(runnable, "diagnostic-report");
                        thread.setDaemon(true);
                        return thread;
                    });

    private static final class Snapshot {
        final File file;
        final String body;

        Snapshot(File file, String body) {
            this.file = file;
            this.body = body;
        }
    }

    private ImportDiagnostics() {}

    static File base(Context context) {
        File external = context.getExternalFilesDir(null);
        return external != null ? external : context.getFilesDir();
    }

    static File reportDirectory(Context context) {
        return new File(context.getFilesDir(), "diagnostic-reports");
    }

    public static void note(Context context, String message) {
        Context application = context.getApplicationContext();
        // Native setup calls this on SDL's worker before that process exits.
        if (android.os.Looper.myLooper() == android.os.Looper.getMainLooper()) {
            REPORTS.execute(() -> writeNote(application, message));
        } else {
            writeNote(application, message);
        }
    }

    private static synchronized void writeNote(Context context, String message) {
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

    private static Snapshot snapshot(Context context)
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
        String body = text.toString();
        return new Snapshot(DiagnosticStore.create(reportDirectory(context), body), body);
    }

    public static void show(Activity activity) {
        prepare(activity, true);
    }

    public static void share(Activity activity) {
        prepare(activity, false);
    }

    private static void prepare(Activity activity, boolean preview) {
        WeakReference<Activity> owner = new WeakReference<>(activity);
        Context context = activity.getApplicationContext();
        Toast.makeText(activity, "Preparing diagnostic report...", Toast.LENGTH_SHORT).show();
        REPORTS.execute(
                () -> {
                    try {
                        Snapshot snapshot = snapshot(context);
                        Activity current = owner.get();
                        if (current != null)
                            current.runOnUiThread(
                                    () -> {
                                        if (current.isFinishing() || current.isDestroyed()) return;
                                        if (preview) showSnapshot(current, snapshot);
                                        else shareSnapshot(current, snapshot.file);
                                    });
                    } catch (IOException | PackageManager.NameNotFoundException error) {
                        Log.e("ImportDiagnostics", "Cannot create report", error);
                        Activity current = owner.get();
                        if (current != null)
                            current.runOnUiThread(
                                    () -> {
                                        if (!current.isFinishing() && !current.isDestroyed())
                                            Toast.makeText(
                                                            current,
                                                            "Cannot create the diagnostic report",
                                                            Toast.LENGTH_LONG)
                                                    .show();
                                    });
                    }
                });
    }

    private static void showSnapshot(Activity activity, Snapshot snapshot) {
        android.widget.TextView text = new android.widget.TextView(activity);
        text.setText(snapshot.body);
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
                .setNeutralButton(
                        "Share", (dialog, which) -> shareSnapshot(activity, snapshot.file))
                .show();
    }

    private static void shareSnapshot(Activity activity, File file) {
        Uri uri =
                new Uri.Builder()
                        .scheme("content")
                        .authority(activity.getPackageName() + ".importdiag")
                        .appendPath(file.getName())
                        .build();
        Intent intent = new Intent(Intent.ACTION_SEND).setType("text/plain");
        intent.putExtra(Intent.EXTRA_STREAM, uri);
        intent.putExtra(Intent.EXTRA_SUBJECT, file.getName());
        intent.setClipData(ClipData.newRawUri("Diagnostic report", uri));
        intent.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
        try {
            activity.startActivity(Intent.createChooser(intent, "Share diagnostic report"));
        } catch (android.content.ActivityNotFoundException error) {
            Toast.makeText(activity, "No application can share this report", Toast.LENGTH_LONG)
                    .show();
        }
    }
}
