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
import android.provider.DocumentsContract;
import android.view.*;
import android.widget.*;

import org.json.*;

import java.io.*;
import java.nio.file.*;
import java.security.*;
import java.util.*;

/** Optional imports, independent of the existing PKG/licence installation. */
public final class OfflineImports {
    private static final int ZIP = 27841, FOLDER = 27842;
    private static boolean busy;

    static boolean supported(Context context) {
        try {
            android.content.pm.ActivityInfo info = context.getPackageManager().getActivityInfo(
                new ComponentName(context, GameActivity.class), android.content.pm.PackageManager.GET_META_DATA);
            return info.metaData != null && "ss-delta".equals(info.metaData.getString("game.library"));
        } catch (android.content.pm.PackageManager.NameNotFoundException error) { return false; }
    }

    public static boolean importedReady(Context context) {
        return supported(context) && new File(context.getFilesDir(), "arm64-import.zip").isFile();
    }

    public static void refreshStatus(Activity activity) {
        ImportScreen.refresh(activity);
    }

    public static void nativeStepResult(Activity activity, int step, int result, Intent data) {
        if (result == Activity.RESULT_OK
                && data != null
                && data.getIntExtra("exit_code", -1) == 0
                && step == 3)
            activity.getSharedPreferences("offline-import-status", 0)
                    .edit()
                    .putBoolean("pkg_installed", true)
                    .apply();
        refreshStatus(activity);
    }

    public static void attach(Activity activity) {
        ImportScreen.refresh(activity);
    }

    static void choose(Activity activity, int choice) {
        if (choice == 2 && activity instanceof BuildActivity) {
            ((BuildActivity) activity).chooseOfficialFirmware();
            return;
        }
        if (busy) {
            Toast.makeText(activity, "An import is still running", Toast.LENGTH_LONG).show();
            return;
        }
        Intent pick =
                new Intent(
                        choice == 1
                                ? Intent.ACTION_OPEN_DOCUMENT_TREE
                                : Intent.ACTION_OPEN_DOCUMENT);
        if (choice != 1) {
            pick.addCategory(Intent.CATEGORY_OPENABLE);
            pick.setType("*/*");
        }
        activity.startActivityForResult(pick, choice == 0 ? ZIP : FOLDER);
    }

    public static boolean result(Activity activity, int request, int result, Intent data) {
        if (request != ZIP && request != FOLDER) return false;
        if (result != Activity.RESULT_OK || data == null) return true;
        if (busy) return true;
        busy = true;
        Context context = activity.getApplicationContext();
        ProgressDialog progress = new ProgressDialog(activity);
        progress.setMessage("Checking and importing files...");
        progress.setCancelable(false);
        progress.show();
        new Thread(
                        () -> {
                            String message;
                            try {
                                {
                                    importModules(context, data.getData(), request == FOLDER);
                                    message =
                                            "10 ARM64 modules verified. The manifest was created"
                                                + " automatically. Continue or retry setup; no"
                                                + " phone compilation is needed.";
                                }
                            } catch (Exception e) {
                                message = "Import failed: " + e.getMessage();
                                android.util.Log.e("OfflineImports", message, e);
                            }
                            final String text = message;
                            activity.runOnUiThread(
                                    () -> {
                                        busy = false;
                                        if (!activity.isFinishing() && !activity.isDestroyed()) {
                                            progress.dismiss();
                                            refreshStatus(activity);
                                            if (activity instanceof BuildActivity)
                                                ((BuildActivity) activity).onModulesImported();
                                            new AlertDialog.Builder(activity)
                                                    .setTitle("Offline imports")
                                                    .setMessage(text)
                                                    .setPositiveButton("OK", null)
                                                    .show();
                                        }
                                    });
                        },
                        "offline-import")
                .start();
        return true;
    }

    private static void mode(Context context, String value) throws IOException {
        File temp = new File(context.getFilesDir(), "arm64-mode.new");
        java.nio.file.Files.write(
                temp.toPath(), value.getBytes(java.nio.charset.StandardCharsets.UTF_8));
        java.nio.file.Files.move(
                temp.toPath(),
                new File(context.getFilesDir(), "arm64-mode").toPath(),
                StandardCopyOption.REPLACE_EXISTING,
                StandardCopyOption.ATOMIC_MOVE);
    }

    private static InputStream open(Context context, Uri uri) throws IOException {
        if (uri == null) throw new IOException("No file was selected");
        InputStream in = context.getContentResolver().openInputStream(uri);
        if (in == null) throw new IOException("Cannot read the selected file");
        return in;
    }

    private static void importModules(Context context, Uri uri, boolean directory)
            throws Exception {
        File stage = new File(context.getCacheDir(), "arm64-stage-" + UUID.randomUUID());
        if (!stage.mkdir()) throw new IOException("Cannot create staging folder");
        File incoming = new File(context.getFilesDir(), "arm64-import.new");
        try {
            if (directory) {
                Uri children =
                        DocumentsContract.buildChildDocumentsUriUsingTree(
                                uri, DocumentsContract.getTreeDocumentId(uri));
                Set<String> seen = new HashSet<>();
                long total = 0;
                try (Cursor cursor =
                        context.getContentResolver()
                                .query(
                                        children,
                                        new String[] {
                                            DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                                            DocumentsContract.Document.COLUMN_DISPLAY_NAME
                                        },
                                        null,
                                        null,
                                        null)) {
                    if (cursor == null) throw new IOException("Cannot read this folder");
                    while (cursor.moveToNext()) {
                        String name = cursor.getString(1);
                        if (!ArmModuleFiles.expected(name)) continue;
                        if (!seen.add(name)) throw new IOException("Duplicate module: " + name);
                        Uri file =
                                DocumentsContract.buildDocumentUriUsingTree(
                                        uri, cursor.getString(0));
                        try (InputStream in = open(context, file)) {
                            total +=
                                    ArmModuleFiles.copy(
                                            in,
                                            new File(stage, name),
                                            Math.min(
                                                    ArmModuleFiles.MAX_FILE,
                                                    ArmModuleFiles.MAX_TOTAL - total));
                        }
                    }
                }
            } else {
                File zip = new File(stage, "source.zip");
                try (InputStream in = open(context, uri)) {
                    ArmModuleFiles.copy(in, zip, ArmModuleFiles.MAX_TOTAL);
                }
                ArmModuleFiles.unpack(zip, stage);
            }
            String generated = ArmModuleFiles.manifest(stage);
            File supplied = new File(stage, "source-manifest.json");
            if (supplied.isFile()) {
                JSONObject old =
                        new JSONObject(
                                new String(
                                        Files.readAllBytes(supplied.toPath()),
                                        java.nio.charset.StandardCharsets.UTF_8));
                JSONObject checked = new JSONObject(generated);
                for (String name : ArmModuleFiles.NAMES)
                    if (!old.getJSONObject(name)
                            .getString("sha256")
                            .equalsIgnoreCase(checked.getJSONObject(name).getString("sha256")))
                        throw new IOException("Manifest hash mismatch: " + name);
            }
            ArmModuleFiles.pack(stage, incoming);
            Files.move(
                    incoming.toPath(),
                    new File(context.getFilesDir(), "arm64-import.zip").toPath(),
                    StandardCopyOption.REPLACE_EXISTING,
                    StandardCopyOption.ATOMIC_MOVE);
            mode(context, "import");
        } finally {
            incoming.delete();
            ArmModuleFiles.removeStage(stage);
        }
    }
}
