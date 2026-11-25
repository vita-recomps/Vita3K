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
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.opengl.EGL14;
import android.opengl.EGLConfig;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.opengl.EGLSurface;
import android.opengl.GLES20;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * Starts the game, after offering a Turnip Vulkan driver on the Adreno GPUs it was tested for
 * (ss-delta 09-android, D25). Qualcomm's own driver lacks coherent framebuffer reads, and games that
 * blend in their shaders (Soul Sacrifice Delta's fog) come out with black rectangles; Mesa's Turnip has
 * them. The offer: a pinned release, checked against its SHA-256, saved as files/drivers/<name>.zip,
 * which the game unpacks into its own storage and uses ([debug] gpu_driver = auto). Asked once per
 * install unless the player chooses "Not now".
 */
public class SetupActivity extends Activity {
    private static final String TAG = "SetupActivity";
    private static final String PREFS = "setup";
    private static final String KEY_DRIVER_ANSWERED = "driver_offer_answered";

    // Eden's recommendation for Adreno 711-799, tested on the Adreno 740 (AYN Thor)
    private static final String DRIVER_NAME = "Turnip T23 (Mr. Purple, Mesa 26.0.0)";
    private static final String DRIVER_FILE = "turnip_mrpurple_T23-toasted.adpkg.zip";
    private static final String DRIVER_URL =
        "https://github.com/MrPurple666/purple-turnip/releases/download/vturnip_mrpurple_T23-toasted.adpkg/turnip_mrpurple_T23-toasted.adpkg.zip";
    private static final String DRIVER_SHA256 = "5d290bc1421af9fbe30ca35afa9e28bb27f03daf3648203afe83a34e56620b82";
    private static final int DRIVER_SIZE = 3558070;

    private final Handler main = new Handler(Looper.getMainLooper());

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        if (buildToolsTest())
            return;
        publishShortcuts();
        // the launcher's "Add an add-on" shortcut (long press on the icon): the setup screen, which
        // installs an add-on into the game built here
        if (ACTION_ADD_ADDON.equals(getIntent() != null ? getIntent().getAction() : null)) {
            startActivity(new Intent(this, BuildActivity.class).putExtra(BuildActivity.EXTRA_ADD_ADDON, true));
            finish();
            return;
        }
        // "Import a Vita save": a picked save, through the game's --import-save (once the game is built)
        if (ACTION_IMPORT_SAVE.equals(getIntent() != null ? getIntent().getAction() : null) && !BuildActivity.needsSetup(this)) {
            startActivity(new Intent(this, SaveImportActivity.class));
            finish();
            return;
        }
        // not built on this device yet, or built by an older version of the app: the setup first
        if (BuildActivity.needsSetup(this)) {
            startActivity(new Intent(this, BuildActivity.class));
            finish();
            return;
        }
        offerDriver(this, this::startGame);
    }

    /**
     * The Turnip offer when it applies (an Adreno 711-799, no package yet, not declined for good), then
     * `then` on the main thread, whatever the answer. The setup asks it before its first run of the game
     * (the module dump), which then runs on the downloaded driver too: Qualcomm's driver on an Adreno 735
     * (Android 16) crashed there (ss-delta #25), so the game never got built. Play asks it again when it
     * wasn't answered then.
     */
    static void offerDriver(Activity activity, Runnable then) {
        if (!shouldOffer(activity)) {
            then.run();
            return;
        }
        new AlertDialog.Builder(activity, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle("GPU driver")
            .setMessage("This device's own Vulkan driver (Qualcomm's) draws some of this game's effects wrong: "
                + "black rectangles in fog and smoke.\n\n"
                + "Turnip, the open-source Mesa driver for Adreno GPUs, draws them correctly. Download it now? "
                + "(" + DRIVER_NAME + ", " + (DRIVER_SIZE / 100000) / 10.0 + " MB from github.com/MrPurple666/purple-turnip, "
                + "MIT licence.) It is used by this game only, from the setup on.")
            .setCancelable(false)
            .setPositiveButton("Download", (d, w) -> download(activity, then))
            .setNeutralButton("Not now", (d, w) -> then.run())
            .setNegativeButton("Don't ask again", (d, w) -> {
                answered(activity);
                then.run();
            })
            .show();
    }

    private static boolean shouldOffer(Context context) {
        if (context.getSharedPreferences(PREFS, MODE_PRIVATE).getBoolean(KEY_DRIVER_ANSWERED, false))
            return false;
        File drivers = driversDir(context);
        String[] present = drivers.list();
        if (present != null) {
            for (String name : present)
                if (name.endsWith(".zip"))
                    return false; // a package is there already (the PC setup, or an earlier download)
        }
        int model = adrenoModel();
        Log.i(TAG, "Adreno model " + model);
        return model >= 711 && model <= 799;
    }

    private static File driversDir(Context context) {
        File base = context.getExternalFilesDir(null);
        return new File(base != null ? base : context.getFilesDir(), "drivers");
    }

    private static void answered(Context context) {
        context.getSharedPreferences(PREFS, MODE_PRIVATE).edit().putBoolean(KEY_DRIVER_ANSWERED, true).apply();
    }

    static final String ACTION_ADD_ADDON = "io.github.vitarecomps.frontend.ADD_ADDON";
    static final String ACTION_IMPORT_SAVE = "io.github.vitarecomps.frontend.IMPORT_SAVE";

    // "Add an add-on" and "Import a Vita save" on the launcher's long-press menu: the only ways back to
    // the setup screen and into the game's folder once the game is built, since the icon starts the game
    // straight away and Android keeps file managers out of an app's folder
    private void publishShortcuts() {
        try {
            android.content.pm.ShortcutManager shortcuts = getSystemService(android.content.pm.ShortcutManager.class);
            if (shortcuts == null)
                return;
            Intent intent = new Intent(this, SetupActivity.class).setAction(ACTION_ADD_ADDON)
                .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TASK | Intent.FLAG_ACTIVITY_NEW_TASK);
            android.content.pm.ShortcutInfo addon = new android.content.pm.ShortcutInfo.Builder(this, "add_addon")
                .setShortLabel("Add an add-on")
                .setLongLabel("Add an add-on (DLC)")
                .setIcon(android.graphics.drawable.Icon.createWithResource("android", android.R.drawable.ic_input_add))
                .setIntent(intent)
                .build();
            Intent importIntent = new Intent(this, SetupActivity.class).setAction(ACTION_IMPORT_SAVE)
                .addFlags(Intent.FLAG_ACTIVITY_CLEAR_TASK | Intent.FLAG_ACTIVITY_NEW_TASK);
            android.content.pm.ShortcutInfo importSave = new android.content.pm.ShortcutInfo.Builder(this, "import_save")
                .setShortLabel("Import a Vita save")
                .setLongLabel("Import a Vita save")
                .setIcon(android.graphics.drawable.Icon.createWithResource("android", android.R.drawable.ic_menu_save))
                .setIntent(importIntent)
                .build();
            shortcuts.setDynamicShortcuts(java.util.Arrays.asList(addon, importSave));
        } catch (Exception e) {
            Log.w(TAG, "shortcuts", e);
        }
    }

    // The GPU's name from an off-screen OpenGL ES context ("Adreno (TM) 740"): its model number, or 0
    private static int adrenoModel() {
        EGLDisplay display = EGL14.eglGetDisplay(EGL14.EGL_DEFAULT_DISPLAY);
        if (display == EGL14.EGL_NO_DISPLAY)
            return 0;
        int[] version = new int[2];
        if (!EGL14.eglInitialize(display, version, 0, version, 1))
            return 0;
        String renderer = null;
        try {
            int[] attributes = { EGL14.EGL_RENDERABLE_TYPE, EGL14.EGL_OPENGL_ES2_BIT, EGL14.EGL_SURFACE_TYPE, EGL14.EGL_PBUFFER_BIT, EGL14.EGL_NONE };
            EGLConfig[] configs = new EGLConfig[1];
            int[] count = new int[1];
            if (!EGL14.eglChooseConfig(display, attributes, 0, configs, 0, 1, count, 0) || count[0] == 0)
                return 0;
            EGLContext context = EGL14.eglCreateContext(display, configs[0], EGL14.EGL_NO_CONTEXT,
                new int[] { EGL14.EGL_CONTEXT_CLIENT_VERSION, 2, EGL14.EGL_NONE }, 0);
            EGLSurface surface = EGL14.eglCreatePbufferSurface(display, configs[0],
                new int[] { EGL14.EGL_WIDTH, 1, EGL14.EGL_HEIGHT, 1, EGL14.EGL_NONE }, 0);
            if (EGL14.eglMakeCurrent(display, surface, surface, context))
                renderer = GLES20.glGetString(GLES20.GL_RENDERER);
            EGL14.eglMakeCurrent(display, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_SURFACE, EGL14.EGL_NO_CONTEXT);
            EGL14.eglDestroySurface(display, surface);
            EGL14.eglDestroyContext(display, context);
        } finally {
            EGL14.eglTerminate(display);
        }
        if (renderer == null)
            return 0;
        Matcher m = Pattern.compile("Adreno[^0-9]*([0-9]{3})").matcher(renderer);
        return m.find() ? Integer.parseInt(m.group(1)) : 0;
    }

    private static void download(Activity activity, Runnable then) {
        AlertDialog progress = new AlertDialog.Builder(activity, android.R.style.Theme_DeviceDefault_Dialog_Alert)
            .setTitle("GPU driver")
            .setMessage("Downloading " + DRIVER_NAME + "…")
            .setCancelable(false)
            .show();
        Handler main = new Handler(Looper.getMainLooper());
        new Thread(() -> {
            String error = fetch(activity);
            main.post(() -> {
                progress.dismiss();
                if (error == null) {
                    answered(activity);
                    then.run();
                    return;
                }
                new AlertDialog.Builder(activity, android.R.style.Theme_DeviceDefault_Dialog_Alert)
                    .setTitle("GPU driver")
                    .setMessage("The download failed: " + error + "\n\nThe game runs with the device's own driver; "
                        + "the offer comes back next time.")
                    .setCancelable(false)
                    .setPositiveButton("OK", (d, w) -> then.run())
                    .show();
            });
        }).start();
    }

    // The pinned package into drivers/, checked; null, or what went wrong
    private static String fetch(Context context) {
        File dir = driversDir(context);
        File part = new File(dir, DRIVER_FILE + ".part");
        try {
            if (!dir.isDirectory() && !dir.mkdirs())
                return "no folder " + dir;
            HttpURLConnection connection = (HttpURLConnection) new URL(DRIVER_URL).openConnection();
            connection.setConnectTimeout(20000);
            connection.setReadTimeout(30000);
            connection.setInstanceFollowRedirects(true);
            MessageDigest sha = MessageDigest.getInstance("SHA-256");
            long total = 0;
            try (InputStream in = connection.getInputStream(); FileOutputStream out = new FileOutputStream(part)) {
                byte[] buffer = new byte[65536];
                for (int n; (n = in.read(buffer)) > 0;) {
                    out.write(buffer, 0, n);
                    sha.update(buffer, 0, n);
                    total += n;
                    if (total > DRIVER_SIZE)
                        break;
                }
            } finally {
                connection.disconnect();
            }
            StringBuilder hex = new StringBuilder();
            for (byte b : sha.digest())
                hex.append(String.format("%02x", b));
            if (total != DRIVER_SIZE || !hex.toString().equals(DRIVER_SHA256)) {
                part.delete();
                return "the file is not the expected one (" + total + " bytes)";
            }
            File done = new File(dir, DRIVER_FILE);
            if (!part.renameTo(done))
                return "could not save " + done;
            return null;
        } catch (Exception e) {
            Log.e(TAG, "driver download", e);
            part.delete();
            return e.getMessage() != null ? e.getMessage() : e.toString();
        }
    }

    // A tiny module built and loaded as the setup builds and the game loads them: imports, zig's version, a
    // shared library from zig's clang (it starts zig again as a child) into the app's storage
    private static final String SELFTEST_PY = String.join("\n",
        "import os, subprocess, sys, time",
        "import capstone, yaml",
        "toolchain, kit, out = sys.argv[1:4]",
        "print('python', sys.version.split()[0], sys.platform, 'executable', sys.executable)",
        "md = capstone.Cs(capstone.CS_ARCH_ARM, capstone.CS_MODE_THUMB)",
        "print('capstone', capstone.__version__, [i.mnemonic for i in md.disasm(bytes([0x70, 0x47]), 0)], 'yaml', yaml.__version__)",
        "zig = os.path.join(toolchain, 'zig', 'zig')",
        "env = dict(os.environ, ZIG_GLOBAL_CACHE_DIR=os.path.join(toolchain, 'cache'), ZIG_LOCAL_CACHE_DIR=os.path.join(toolchain, 'cache'))",
        "print('zig', subprocess.run([zig, 'version'], capture_output=True, text=True, env=env).stdout.strip())",
        "src = os.path.join(os.path.dirname(out), 'selftest.c')",
        "open(src, 'w').write('int selftest_answer(void) { return 42; }\\n')",
        "t = time.time()",
        "r = subprocess.run([zig, 'cc', '-target', 'aarch64-linux-musl', '-shared', '-nostdlib', '-o', out, src], capture_output=True, text=True, env=env)",
        "print('zig cc', r.returncode, f'{time.time() - t:.1f} s', r.stderr.strip()[-500:])",
        "sys.exit(r.returncode)", "");

    // Test tools only, in debuggable builds (ss-delta 14g):
    //   adb shell am start -n <package>/io.github.vitarecomps.frontend.SetupActivity --es buildtools selftest
    //   ... --es buildtools python --es args "<arguments; {kit} and {toolchain} name those folders>"
    //   ... --es buildtools frontend --es args "<the front end's arguments>" (FrontendStepActivity)
    // runs the build tools, or a step of the front end, and logs to buildtools-test.log in the app's
    // external folder
    private boolean buildToolsTest() {
        Intent intent = getIntent();
        String mode = intent != null ? intent.getStringExtra("buildtools") : null;
        if (mode == null || (getApplicationInfo().flags & android.content.pm.ApplicationInfo.FLAG_DEBUGGABLE) == 0)
            return false;
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        android.widget.TextView view = new android.widget.TextView(this);
        view.setText("build tools test: " + mode);
        setContentView(view);
        String args = intent.getStringExtra("args");
        File base = getExternalFilesDir(null);
        File logFile = new File(base != null ? base : getFilesDir(), "buildtools-test.log");
        if (mode.equals("frontend")) {
            // one of the front end's steps (FrontendStepActivity); its exit code arrives in onActivityResult
            testLog = logFile;
            testStart = System.currentTimeMillis();
            testLog("== frontend " + args);
            startActivityForResult(new Intent(this, FrontendStepActivity.class)
                .putExtra("args", (args != null ? args : "").trim().split("\\s+")), REQUEST_FRONTEND_TEST);
            return true;
        }
        new Thread(() -> {
            long start = System.currentTimeMillis();
            try (java.io.PrintWriter log = new java.io.PrintWriter(new java.io.FileWriter(logFile, true), true)) {
                java.util.function.Consumer<String> say = line -> {
                    String stamped = String.format(java.util.Locale.ROOT, "%7.1f %s", (System.currentTimeMillis() - start) / 1000.0, line);
                    log.println(stamped);
                    Log.i(TAG, stamped);
                };
                say.accept("== " + mode + (args != null ? " " + args : ""));
                try {
                    BuildTools.unpack(this, say);
                    say.accept("unpacked or current");
                    String kit = BuildTools.kit(this).getPath();
                    String toolchain = BuildTools.toolchain(this).getPath();
                    int code;
                    if (mode.equals("selftest")) {
                        File script = new File(getCacheDir(), "selftest.py");
                        File so = new File(getCacheDir(), "libselftest.so");
                        try (FileOutputStream out = new FileOutputStream(script)) {
                            out.write(SELFTEST_PY.getBytes(java.nio.charset.StandardCharsets.UTF_8));
                        }
                        code = BuildTools.run(BuildTools.python(this, java.util.Arrays.asList(script.getPath(), toolchain, kit, so.getPath())), say);
                        if (code == 0) {
                            System.load(so.getPath());
                            say.accept("loaded " + so + " into the app");
                        }
                    } else {
                        java.util.List<String> list = new java.util.ArrayList<>();
                        for (String a : (args != null ? args : "").trim().split("\\s+"))
                            if (!a.isEmpty())
                                list.add(a.replace("{kit}", kit).replace("{toolchain}", toolchain));
                        code = BuildTools.run(BuildTools.python(this, list), say);
                    }
                    say.accept("exit " + code);
                } catch (Throwable e) {
                    say.accept("failed: " + e);
                    Log.e(TAG, "build tools test", e);
                }
            } catch (IOException e) {
                Log.e(TAG, "build tools test log", e);
            }
            main.post(this::finish);
        }).start();
        return true;
    }

    private static final int REQUEST_FRONTEND_TEST = 1;
    private File testLog;
    private long testStart;

    private void testLog(String line) {
        String stamped = String.format(java.util.Locale.ROOT, "%7.1f %s", (System.currentTimeMillis() - testStart) / 1000.0, line);
        Log.i(TAG, stamped);
        try (java.io.FileWriter out = new java.io.FileWriter(testLog, true)) {
            out.write(stamped + "\n");
        } catch (IOException e) {
            Log.e(TAG, "test log", e);
        }
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_FRONTEND_TEST && testLog != null) {
            int code = data != null ? data.getIntExtra(FrontendStepActivity.EXTRA_EXIT_CODE, -1) : -1;
            testLog("frontend step result " + resultCode + ", exit " + code);
            finish();
        }
    }

    private void startGame() {
        Intent intent = new Intent(this, GameActivity.class);
        Intent own = getIntent();
        if (own != null && own.getExtras() != null)
            intent.putExtras(own.getExtras());
        startActivity(intent);
        finish();
    }
}
