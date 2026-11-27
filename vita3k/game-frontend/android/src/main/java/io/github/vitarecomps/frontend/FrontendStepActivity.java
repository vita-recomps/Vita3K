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
import android.content.Intent;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import org.libsdl.app.SDLActivity;

/**
 * One of the front end's own setup steps, run as the desktop setup runs the executable (ss-delta D27):
 * --install-firmware PUP, --install-content dump [--license ...], --dump-elfs seconds. Started for a
 * result with the "args" string array, as GameActivity takes it; the result code is the front end's
 * exit code (RESULT_OK for 0, else RESULT_FIRST_USER + code), and the "exit_code" extra holds it.
 *
 * It runs in a process of its own (the manifest's :frontend_step), which ends with the step: the
 * front end keeps state in static objects and is never run twice in one process.
 */
public class FrontendStepActivity extends GameActivity {
    private static final String TAG = "FrontendStep";
    public static final String EXTRA_EXIT_CODE = "exit_code";
    // picked files up to this size (licences, a small mod) are copied into the cache, bigger ones linked
    private static final long SMALL_FILE = 16L * 1024 * 1024;

    // the files the player picked stay open while the step runs
    private final java.util.List<android.os.ParcelFileDescriptor> picked = new java.util.ArrayList<>();

    /**
     * The step's arguments, with each content:// URI (a file the player picked through Android's file
     * picker) as a path the native code can open: a link in the cache, named with the document's
     * extension (the front end tells a .pkg from a .zip by it), to /proc/self/fd/N of the open file. A
     * 3 GB dump is read in place, never copied; a small file (a licence) is copied there instead.
     */
    @Override
    protected String[] getArguments() {
        String[] args = super.getArguments();
        for (int i = 0; i < args.length; i++) {
            if (!args[i].startsWith("content://"))
                continue;
            android.net.Uri uri = android.net.Uri.parse(args[i]);
            try {
                android.os.ParcelFileDescriptor fd = getContentResolver().openFileDescriptor(uri, "r");
                if (fd == null)
                    continue;
                String name = displayName(uri);
                int dot = name.lastIndexOf('.');
                String extension = dot >= 0 ? name.substring(dot).toLowerCase(java.util.Locale.ROOT) : "";
                // named as the document, so the front end's messages (step-error.txt) name what was
                // picked; one folder per argument keeps two files of the same name apart
                String base = (dot >= 0 ? name.substring(0, dot) : name).replaceAll("[^A-Za-z0-9._ ()\\[\\]-]", "_");
                java.io.File dir = new java.io.File(getCacheDir(), "picked-" + i);
                dir.mkdirs();
                java.io.File[] old = dir.listFiles();
                if (old != null)
                    for (java.io.File f : old)
                        f.delete();
                java.io.File link = new java.io.File(dir, (base.isEmpty() ? "picked" : base) + extension);
                long size = fd.getStatSize();
                if (size >= 0 && size <= SMALL_FILE) {
                    // a licence: copied, as the native code reads it with a stream, which can't take
                    // the descriptor
                    try (java.io.InputStream in = new android.os.ParcelFileDescriptor.AutoCloseInputStream(fd);
                         java.io.OutputStream out = new java.io.FileOutputStream(link)) {
                        byte[] buffer = new byte[64 * 1024];
                        for (int n; (n = in.read(buffer)) > 0;)
                            out.write(buffer, 0, n);
                    }
                    Log.i(TAG, "picked " + name + " (" + size + " bytes) copied to " + link);
                } else {
                    // the native code's FOPEN reads through the descriptor behind the link: opening
                    // the link would open the file again with the app's own permissions, which it
                    // lacks for shared storage on some devices (ss-delta #23)
                    picked.add(fd);
                    android.system.Os.symlink("/proc/self/fd/" + fd.getFd(), link.getPath());
                    Log.i(TAG, "picked " + name + " (" + size + " bytes) as " + link);
                }
                args[i] = link.getPath();
            } catch (Exception e) {
                Log.e(TAG, "can't open " + uri, e);
            }
        }
        return args;
    }

    private String displayName(android.net.Uri uri) {
        try (android.database.Cursor c = getContentResolver().query(uri, new String[] { android.provider.OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (c != null && c.moveToFirst() && c.getString(0) != null)
                return c.getString(0);
        } catch (Exception e) {
            Log.w(TAG, "no name for " + uri, e);
        }
        String path = uri.getLastPathSegment();
        return path != null ? path : "";
    }

    @Override
    protected void main() {
        String library = getMainSharedObject();
        String function = getMainFunction();
        String[] arguments = getArguments();
        Log.i(TAG, "step: " + String.join(" ", arguments));
        int code;
        try {
            code = SDLActivity.nativeRunMain(library, function, arguments);
        } catch (RuntimeException | LinkageError error) {
            Log.e(TAG, "Native setup launch failed", error);
            ImportDiagnostics.note(this, "Native setup launch failed: " + Log.getStackTraceString(error));
            code = 1;
        }
        Log.i(TAG, "step ended with " + code);
        // set before SDL's thread calls finish(), which hands the result to the caller
        setResult(code == 0 ? Activity.RESULT_OK : Activity.RESULT_FIRST_USER + code,
            new Intent().putExtra(EXTRA_EXIT_CODE, code));
    }

    @Override
    protected void onDestroy() {
        super.onDestroy();
        if (isFinishing()) {
            // the result is with the system once finish() returned; a fresh process for the next step
            new Handler(Looper.getMainLooper()).postDelayed(() -> android.os.Process.killProcess(android.os.Process.myPid()), 300);
        }
    }
}
