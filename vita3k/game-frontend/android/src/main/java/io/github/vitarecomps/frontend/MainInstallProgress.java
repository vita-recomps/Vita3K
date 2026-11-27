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

import java.io.File;
import java.util.*;

/** A read-only setup monitor; it does not move the game or extraction onto another thread. */
public final class MainInstallProgress {
    private static final Map<Activity, Tracker> active = new WeakHashMap<>();

    private static long identity(File file) {
        try {
            return android.system.Os.stat(file.getPath()).st_ino;
        } catch (Exception e) {
            return -1;
        }
    }

    public static void begin(Activity activity, String[] args) {
        boolean install =
                Arrays.asList(args).contains("--install-content")
                        || Arrays.asList(args).contains("--install-firmware");
        if (!install) return;
        stop(activity);
        File base = activity.getExternalFilesDir(null);
        if (base == null) base = activity.getFilesDir();
        ImportScreen.progress(activity, true, -1, "Preparing files...");
        ImportScreen.setBusy(activity, true);
        Tracker tracker = new Tracker(activity, new File(base, "ss-delta/vita3k.log"));
        active.put(activity, tracker);
        tracker.start();
    }

    public static void stop(Activity activity) {
        Tracker tracker = active.remove(activity);
        if (tracker != null) {
            tracker.stopped = true;
            tracker.interrupt();
        }
        ImportScreen.progress(activity, false, -1, "");
    }

    private static final class Tracker extends Thread {
        private final Activity activity;
        private final File log;
        private final InstallProgressLog reader;
        volatile boolean stopped;

        Tracker(Activity activity, File log) {
            super("setup-progress");
            this.activity = activity;
            this.log = log;
            reader = new InstallProgressLog(log, identity(log));
        }

        public void run() {
            while (!stopped) {
                try {
                    reader.poll(identity(log));
                    final int percent = reader.percent;
                    final String text = reader.detail;
                    activity.runOnUiThread(
                            () -> {
                                if (!stopped && !activity.isFinishing() && !activity.isDestroyed())
                                    ImportScreen.progress(activity, true, percent, text);
                            });
                    Thread.sleep(500);
                } catch (InterruptedException e) {
                    return;
                } catch (Exception e) {
                    try {
                        Thread.sleep(1000);
                    } catch (InterruptedException ignored) {
                        return;
                    }
                }
            }
        }
    }
}
