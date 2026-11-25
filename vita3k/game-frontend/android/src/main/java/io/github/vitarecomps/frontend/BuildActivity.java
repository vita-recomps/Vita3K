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

import android.Manifest;
import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.database.Cursor;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.OpenableColumns;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.View;
import android.view.WindowManager;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import org.json.JSONArray;
import org.json.JSONObject;

import java.io.File;
import java.io.FileInputStream;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;

/**
 * The game's setup on the device (ss-delta D27, 09-android): the player picks their own dump, and the app
 * builds the game from it as the desktop setups do. The steps, each started when the one before
 * succeeded, and remembered across restarts (a failed one can be tried again):
 *
 *   firmware   Sony's firmware downloaded (BuildService: android-setup.py firmware)
 *   install    the firmware, then the picked dump installed (FrontendStepActivity: the front end's
 *              --install-firmware, --install-content)
 *   check      the installed eboot.bin is a version the port supports (android-setup.py check)
 *   dump       the game run briefly to write its modules (the front end's --dump-elfs)
 *   build      the modules recompiled and compiled (android-setup.py build; minutes, in a foreground
 *              service the player may leave)
 *
 * SetupActivity opens it while the game isn't built here, or when the modules were built by an older
 * version of the app (android-build.json's stamp: the APK's version and its tools). Once the game is
 * built, the launcher's "Add an add-on" shortcut opens it for one more step on its own:
 *
 *   add-on     an add-on (DLC) or a rePatch mod (a .zip) the player picks installed into the game built
 *              here (the front end's --install-content), with nothing built again
 */
public class BuildActivity extends Activity {
    private static final String TAG = "BuildActivity";
    private static final String PREFS = "build";
    private static final String KEY_STEP = "step", KEY_PARTS = "parts", KEY_ADDON = "addon";
    static final String EXTRA_ADD_ADDON = "add_addon";
    private static final int REQUEST_PICK_PART = 1, REQUEST_PICK_LICENSE = 2, REQUEST_STEP = 3,
        REQUEST_PICK_ADDON = 4, REQUEST_PICK_ADDON_LICENSE = 5;

    private static final int PICK = 0, FIRMWARE = 1, INSTALL_FIRMWARE = 2, INSTALL_GAME = 3, CHECK = 4, DUMP = 5, BUILD = 6,
        INSTALL_ADDON = 7;
    private static final String[] STEP_NAMES = { "", "Downloading the firmware", "Installing the firmware",
        "Installing the game", "Checking the game's version", "Running the game to find its code", "Building the native code",
        "Installing the add-on" };

    static volatile boolean visible = false;

    private final Handler main = new Handler(Looper.getMainLooper());
    private TextView partsView, stepView, errorView, logView;
    private final List<View> formViews = new ArrayList<>(); // the intro and the file picking: hidden while setting up
    private Button addButton, licenseButton, clearButton, buildButton, retryButton, playButton, startOverButton, addonButton;
    private ScrollView logScroll;
    private int step;
    private JSONArray parts;
    private JSONObject addon; // the add-on being installed (INSTALL_ADDON): uri, name, license
    private String notice; // what the last add-on step did, shown while the game is built
    private long serviceGeneration = -1; // the BuildService command this screen waits for
    private boolean rebuild; // the game is installed and its modules dumped: only the build is needed
    private JSONObject game; // game.json
    private boolean waitingForStep = false;

    // --- whether the game needs setting up --------------------------------------------------------

    static File gameDir(Context context) {
        File base = context.getExternalFilesDir(null);
        return base != null ? base : context.getFilesDir();
    }

    /** What built the modules: this APK's version and its build tools. */
    static String stamp(Context context) {
        String tools = "";
        try (InputStream in = context.getAssets().open("buildtools.json")) {
            tools = new JSONObject(new String(readAll(in), StandardCharsets.UTF_8)).optString("zip_sha256");
        } catch (Exception e) {
            Log.w(TAG, "buildtools.json", e);
        }
        long version = 0;
        try {
            version = context.getPackageManager().getPackageInfo(context.getPackageName(), 0).getLongVersionCode();
        } catch (PackageManager.NameNotFoundException e) {
            Log.w(TAG, "version", e);
        }
        return version + "-" + tools;
    }

    /**
     * The game isn't installed or has no native modules; or this app built them and has changed since;
     * or a setup is under way. Modules a PC put there (no android-build.json) are left alone. Never
     * true for an APK without the build tools.
     */
    static boolean needsSetup(Context context) {
        if (!BuildTools.present(context))
            return false;
        if (context.getSharedPreferences(PREFS, MODE_PRIVATE).getInt(KEY_STEP, PICK) != PICK)
            return true;
        File game = gameDir(context);
        boolean installed = false;
        File[] apps = new File(game, "fs/ux0/app").listFiles();
        if (apps != null)
            for (File app : apps)
                installed |= new File(app, "eboot.bin").isFile();
        String[] modules = game.list((dir, name) -> name.startsWith("ssd_aot_") && name.endsWith(".so"));
        if (!installed || modules == null || modules.length == 0)
            return true;
        File record = new File(game, "android-build.json");
        if (!record.isFile())
            return false;
        try (InputStream in = new FileInputStream(record)) {
            return !new JSONObject(new String(readAll(in), StandardCharsets.UTF_8)).optString("stamp").equals(stamp(context));
        } catch (Exception e) {
            return true;
        }
    }

    // --- the screen ---------------------------------------------------------------------------

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        SharedPreferences prefs = getSharedPreferences(PREFS, MODE_PRIVATE);
        step = prefs.getInt(KEY_STEP, PICK);
        try {
            parts = new JSONArray(prefs.getString(KEY_PARTS, "[]"));
        } catch (Exception e) {
            parts = new JSONArray();
        }
        try {
            addon = prefs.contains(KEY_ADDON) ? new JSONObject(prefs.getString(KEY_ADDON, "{}")) : null;
        } catch (Exception e) {
            addon = null;
        }

        int pad = dp(24);
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setPadding(pad, pad, pad, pad);

        TextView title = text("Set up " + gameName(), 26, true);
        column.addView(title);
        rebuild = canRebuild();
        TextView intro = text(rebuild
            ? "This version of the app builds the game's native code again, from the game installed here: about "
              + buildMinutes() + " minutes on a fast device, with 5 GB of free space while it runs. Keep the device charging. Your "
              + "dump isn't needed (add it only to install the game again)."
            : "The game is built on this device from your own copy of it: a dump of the PS Vita game (a NoNpDrm .zip "
              + "or .vpk; zip a dumped folder first), or a .pkg with its licence file (work.bin or .rif). Add the "
              + "game's last update after it if the dump lacks it (its .pkg), and add-ons (DLC) the same way, or a "
              + "rePatch mod (a .zip with its rePatch folder); add-ons and mods can also be added later, from the "
              + "app icon's long-press menu. The PS Vita firmware comes from Sony's "
              + "servers (190 MB).\n\n"
              + "Building takes about " + buildMinutes() + " minutes on a fast device and needs about 10 GB of free space (5 GB of it "
              + "only while building). Keep the device charging. Nothing leaves the device.", 15, false);
        intro.setPadding(0, dp(8), 0, dp(12));
        column.addView(intro);
        formViews.add(intro);

        LinearLayout buttons = new LinearLayout(this);
        buttons.setOrientation(LinearLayout.HORIZONTAL);
        addButton = button("Add the game's files…", v -> pick(REQUEST_PICK_PART));
        licenseButton = button("Add a licence…", v -> pick(REQUEST_PICK_LICENSE));
        clearButton = button("Clear", v -> {
            parts = new JSONArray();
            save();
            refresh();
        });
        buttons.addView(addButton);
        buttons.addView(licenseButton);
        buttons.addView(clearButton);
        column.addView(buttons);
        formViews.add(buttons);

        partsView = text("", 15, false);
        partsView.setPadding(0, dp(8), 0, dp(8));
        column.addView(partsView);

        LinearLayout actions = new LinearLayout(this);
        actions.setOrientation(LinearLayout.HORIZONTAL);
        buildButton = button("Build", v -> startSetup());
        retryButton = button("Try again", v -> {
            BuildService.state.error = null;
            run();
        });
        playButton = button("Play", v -> play());
        startOverButton = button("Start over", v -> {
            step = PICK;
            save();
            synchronized (BuildService.state) {
                BuildService.state.error = null;
            }
            refresh();
        });
        addonButton = button("Add an add-on or mod…", v -> pick(REQUEST_PICK_ADDON));
        actions.addView(buildButton);
        actions.addView(retryButton);
        actions.addView(playButton);
        actions.addView(addonButton);
        actions.addView(startOverButton);
        column.addView(actions);

        stepView = text("", 18, true);
        stepView.setPadding(0, dp(12), 0, dp(4));
        column.addView(stepView);
        errorView = text("", 15, false);
        errorView.setTextColor(0xffff6b6b);
        column.addView(errorView);

        logView = new TextView(this);
        logView.setTypeface(Typeface.MONOSPACE);
        logView.setTextSize(TypedValue.COMPLEX_UNIT_SP, 11);
        logView.setTextColor(0xffb0b0b0);
        logScroll = new ScrollView(this);
        logScroll.setFocusable(false); // the gamepad moves between the buttons only
        logScroll.addView(logView);
        column.addView(logScroll, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, dp(220)));

        ScrollView page = new ScrollView(this);
        page.setFocusable(false);
        page.addView(column);
        setContentView(page);
        // the app was closed during one of the front end's steps: the player goes on with Try again
        if (step == INSTALL_FIRMWARE || step == INSTALL_GAME || step == DUMP || step == INSTALL_ADDON) {
            synchronized (BuildService.state) {
                if (BuildService.state.error == null)
                    BuildService.state.error = STEP_NAMES[step] + " was interrupted: Try again goes on from there.";
            }
        }
        refresh();
        boolean built = step == PICK && !needsSetup(this);
        // from the "Add an add-on" shortcut, with the game built: the file picker straight away
        if (built && savedInstanceState == null && getIntent().getBooleanExtra(EXTRA_ADD_ADDON, false))
            pick(REQUEST_PICK_ADDON);
        // where a gamepad's first press lands (a focus requested before layout, in touch mode, is lost)
        page.post(() -> (built ? addonButton : parts.length() == 0 ? addButton : buildButton).requestFocus());
    }

    @Override
    protected void onResume() {
        super.onResume();
        visible = true;
        main.post(poll);
    }

    @Override
    protected void onPause() {
        super.onPause();
        visible = false;
        main.removeCallbacks(poll);
    }

    private final Runnable poll = new Runnable() {
        @Override
        public void run() {
            onServiceProgress();
            main.postDelayed(this, 500);
        }
    };

    private int dp(int v) {
        return Math.round(v * getResources().getDisplayMetrics().density);
    }

    private TextView text(String s, int sp, boolean bold) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        if (bold)
            t.setTypeface(Typeface.DEFAULT_BOLD);
        return t;
    }

    private Button button(String label, View.OnClickListener onClick) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setOnClickListener(onClick);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT);
        lp.setMarginEnd(dp(8));
        b.setLayoutParams(lp);
        return b;
    }

    private void refresh() {
        boolean busy = step != PICK;
        boolean running;
        String error, serviceStep;
        List<String> lines;
        synchronized (BuildService.state) {
            running = BuildService.state.running;
            error = BuildService.state.error;
            serviceStep = BuildService.state.step;
            lines = new ArrayList<>(BuildService.state.lines);
        }
        StringBuilder list = new StringBuilder();
        for (int i = 0; i < parts.length(); i++) {
            JSONObject p = parts.optJSONObject(i);
            list.append(i + 1).append(". ").append(p.optString("name"));
            if (!p.optString("license_name").isEmpty())
                list.append("  (licence: ").append(p.optString("license_name")).append(")");
            list.append('\n');
        }
        if (step == INSTALL_ADDON && addon != null)
            partsView.setText("From:\n" + addon.optString("name")
                + (addon.optString("license_name").isEmpty() ? "" : "  (licence: " + addon.optString("license_name") + ")"));
        else
            partsView.setText(parts.length() == 0 ? "No files chosen yet." : (busy ? "From:\n" : "") + list.toString().trim());
        boolean lastIsPkg = parts.length() > 0 && parts.optJSONObject(parts.length() - 1).optString("name").toLowerCase().endsWith(".pkg");
        addButton.setEnabled(!busy);
        licenseButton.setEnabled(!busy && lastIsPkg);
        clearButton.setEnabled(!busy && parts.length() > 0);
        buildButton.setEnabled(!busy && (parts.length() > 0 || rebuild));
        // built: Play (or Build, to build again); setting up: the progress, and the files it uses (none
        // for a rebuild after an update)
        boolean built = !busy && !needsSetup(this);
        for (View v : formViews)
            v.setVisibility(busy || built ? View.GONE : View.VISIBLE);
        partsView.setVisibility(step == INSTALL_ADDON ? View.VISIBLE
            : built || (busy && parts.length() == 0) ? View.GONE : View.VISIBLE);
        buildButton.setVisibility(busy ? View.GONE : View.VISIBLE);
        addonButton.setVisibility(built ? View.VISIBLE : View.GONE);
        boolean stopped = busy && !running && !waitingForStep && error != null;
        retryButton.setVisibility(stopped ? View.VISIBLE : View.GONE);
        startOverButton.setVisibility(busy && !running && !waitingForStep ? View.VISIBLE : View.GONE);
        playButton.setVisibility(View.GONE);
        if (!busy)
            stepView.setText(needsSetup(this) ? "" : notice != null ? notice : "The game is built.");
        else if (running && step == BUILD)
            stepView.setText(serviceStep);
        else
            stepView.setText(STEP_NAMES[step] + (stopped ? ": stopped" : "…"));
        if (!busy && !needsSetup(this))
            playButton.setVisibility(View.VISIBLE);
        errorView.setText(busy && error != null ? error : "");
        StringBuilder log = new StringBuilder();
        for (int i = Math.max(0, lines.size() - 60); i < lines.size(); i++)
            log.append(lines.get(i)).append('\n');
        logView.setText(log.toString());
        logScroll.post(() -> logScroll.fullScroll(View.FOCUS_DOWN));
    }

    private void save() {
        SharedPreferences.Editor edit = getSharedPreferences(PREFS, MODE_PRIVATE).edit().putInt(KEY_STEP, step)
            .putString(KEY_PARTS, parts.toString());
        if (addon != null)
            edit.putString(KEY_ADDON, addon.toString());
        else
            edit.remove(KEY_ADDON);
        edit.apply();
    }

    /** The add-on picked (and, for a pkg, its licence): installed into the game built here. */
    private void startAddon() {
        BuildService.logFile(this).delete();
        synchronized (BuildService.state) {
            BuildService.state.lines.clear();
            BuildService.state.error = null;
        }
        notice = null;
        step = INSTALL_ADDON;
        save();
        run();
    }

    // --- picking the dump ---------------------------------------------------------------------

    private void pick(int request) {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType("*/*");
        startActivityForResult(intent, request);
    }

    private String displayName(Uri uri) {
        try (Cursor c = getContentResolver().query(uri, new String[] { OpenableColumns.DISPLAY_NAME }, null, null, null)) {
            if (c != null && c.moveToFirst() && c.getString(0) != null)
                return c.getString(0);
        } catch (Exception e) {
            Log.w(TAG, "name", e);
        }
        return uri.getLastPathSegment();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        super.onActivityResult(requestCode, resultCode, data);
        if (requestCode == REQUEST_STEP) {
            waitingForStep = false;
            int code = data != null ? data.getIntExtra(FrontendStepActivity.EXTRA_EXIT_CODE, -1) : -1;
            BuildService.append(this, STEP_NAMES[step] + ": exit " + code);
            if (step == INSTALL_ADDON && resultCode == RESULT_OK && code == 0) {
                // the game stays as it was built: back to Play, with what was installed
                notice = "Installed: " + (addon != null ? addon.optString("name") : "the add-on")
                    + ". The game is built.";
                addon = null;
                step = PICK;
                save();
                refresh();
                playButton.requestFocus();
                return;
            }
            if (resultCode == RESULT_OK && code == 0) {
                step++;
                save();
                run();
            } else {
                synchronized (BuildService.state) {
                    BuildService.state.error = STEP_NAMES[step] + " failed (the game's log: " + game().optString("id")
                        + "/vita3k.log in the app's folder).";
                }
                refresh();
            }
            return;
        }
        boolean picked = resultCode == RESULT_OK && data != null && data.getData() != null;
        // a pkg add-on's licence not given: installed anyway, with the licence the game's install left
        // (the front end says when there is none)
        if (requestCode == REQUEST_PICK_ADDON_LICENSE && !picked && addon != null) {
            startAddon();
            return;
        }
        if (!picked)
            return;
        Uri uri = data.getData();
        try {
            getContentResolver().takePersistableUriPermission(uri, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (SecurityException e) {
            Log.w(TAG, "no persistable permission for " + uri, e);
        }
        try {
            if (requestCode == REQUEST_PICK_ADDON) {
                addon = new JSONObject().put("uri", uri.toString()).put("name", displayName(uri));
                if (addon.optString("name").toLowerCase(java.util.Locale.ROOT).endsWith(".pkg")) {
                    // a pkg needs its licence: the picker again, for its work.bin or .rif
                    notice = "Now pick the licence of " + addon.optString("name") + " (work.bin or .rif).";
                    refresh();
                    pick(REQUEST_PICK_ADDON_LICENSE);
                } else
                    startAddon();
                return;
            } else if (requestCode == REQUEST_PICK_ADDON_LICENSE && addon != null) {
                addon.put("license", uri.toString()).put("license_name", displayName(uri));
                startAddon();
                return;
            } else if (requestCode == REQUEST_PICK_PART) {
                parts.put(new JSONObject().put("uri", uri.toString()).put("name", displayName(uri)));
            } else if (requestCode == REQUEST_PICK_LICENSE && parts.length() > 0) {
                JSONObject last = parts.getJSONObject(parts.length() - 1);
                last.put("license", uri.toString()).put("license_name", displayName(uri));
            }
        } catch (Exception e) {
            Log.e(TAG, "parts", e);
        }
        save();
        refresh();
        buildButton.requestFocus();
    }

    // --- the steps ----------------------------------------------------------------------------

    private void startSetup() {
        if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
            requestPermissions(new String[] { Manifest.permission.POST_NOTIFICATIONS }, 0);
        BuildService.logFile(this).delete();
        synchronized (BuildService.state) {
            BuildService.state.lines.clear();
            BuildService.state.error = null;
        }
        // after an app update, the installed game and its dumped modules are still here: the build alone
        step = parts.length() == 0 && rebuild ? BUILD : FIRMWARE;
        save();
        run();
    }

    private File work() {
        return new File(getFilesDir(), "setup-work");
    }

    /** Starts the current step. */
    private void run() {
        synchronized (BuildService.state) {
            BuildService.state.error = null;
        }
        File game = gameDir(this);
        switch (step) {
        case FIRMWARE:
            service(new String[] { "firmware", work().getPath() });
            break;
        case INSTALL_FIRMWARE: {
            File fw = new File(work(), "firmware");
            frontend(new String[] { "--install-firmware", new File(fw, "PSVUPDAT.PUP").getPath(),
                "--install-firmware", new File(fw, "PSP2UPDAT.PUP").getPath() });
            break;
        }
        case INSTALL_GAME: {
            List<String> args = new ArrayList<>();
            for (int i = 0; i < parts.length(); i++) {
                JSONObject p = parts.optJSONObject(i);
                args.add("--install-content");
                args.add(p.optString("uri"));
                if (!p.optString("license").isEmpty()) {
                    args.add("--license");
                    args.add(p.optString("license"));
                }
            }
            frontend(args.toArray(new String[0]));
            break;
        }
        case CHECK:
            service(new String[] { "check", game.getPath() });
            break;
        case DUMP:
            // the first run of the game: on an Adreno, Turnip is offered before it, not only at Play (#25)
            SetupActivity.offerDriver(this, () -> frontend(new String[] { "--dump-elfs", String.valueOf(dumpSeconds()) }));
            break;
        case BUILD:
            service(new String[] { "build", game.getPath(), work().getPath(), BuildTools.toolchain(this).getPath(), "--stamp", stamp(this) });
            break;
        case INSTALL_ADDON: {
            if (addon == null) {
                step = PICK;
                save();
                break;
            }
            List<String> args = new ArrayList<>();
            args.add("--install-content");
            args.add(addon.optString("uri"));
            if (!addon.optString("license").isEmpty()) {
                args.add("--license");
                args.add(addon.optString("license"));
            }
            frontend(args.toArray(new String[0]));
            break;
        }
        default:
            step = PICK;
            save();
        }
        refresh();
    }

    /** game.json, the tools' zip's first entry (read without unpacking the rest). */
    private JSONObject game() {
        if (game != null)
            return game;
        game = new JSONObject();
        try (java.util.zip.ZipInputStream zip = new java.util.zip.ZipInputStream(getAssets().open("buildtools.zip"))) {
            for (java.util.zip.ZipEntry e; (e = zip.getNextEntry()) != null;)
                if (e.getName().equals("game.json")) {
                    game = new JSONObject(new String(readAll(zip), StandardCharsets.UTF_8));
                    break;
                }
        } catch (Exception e) {
            Log.w(TAG, "game.json", e);
        }
        return game;
    }

    /** How long the build takes on a fast device (game.json android_build_minutes; ss-delta's 15 when absent). */
    private int buildMinutes() {
        return game().optInt("android_build_minutes", 15);
    }

    private String gameName() {
        String name = game().optString("name");
        return !name.isEmpty() ? name : getString(getApplicationInfo().labelRes);
    }

    /** The game is installed here and its modules were dumped: an update needs the build step alone. */
    private boolean canRebuild() {
        File dir = gameDir(this);
        List<String> titles = new ArrayList<>();
        titles.add(game().optString("title_id"));
        JSONArray others = game().optJSONArray("other_title_ids");
        for (int i = 0; others != null && i < others.length(); i++)
            titles.add(others.optString(i));
        for (String title : titles) {
            if (title.isEmpty() || !new File(dir, "fs/ux0/app/" + title + "/eboot.bin").isFile())
                continue;
            String[] dumps = new File(dir, game().optString("id") + "/elfdumps/" + title).list((d, n) -> n.endsWith("_eboot.elf"));
            if (dumps != null && dumps.length > 0)
                return true;
        }
        return false;
    }

    private int dumpSeconds() {
        try (InputStream in = new FileInputStream(new File(BuildTools.root(this), "game.json"))) {
            return new JSONObject(new String(readAll(in), StandardCharsets.UTF_8)).optInt("dump_seconds", 45);
        } catch (Exception e) {
            return 45;
        }
    }

    private void service(String[] args) {
        synchronized (BuildService.state) {
            serviceGeneration = BuildService.state.generation + 1;
        }
        Intent intent = new Intent(this, BuildService.class).putExtra(BuildService.EXTRA_ARGS, args)
            .putExtra(BuildService.EXTRA_TITLE, STEP_NAMES[step]);
        startForegroundService(intent);
    }

    private void frontend(String[] args) {
        BuildService.append(this, "$ front end " + String.join(" ", args));
        waitingForStep = true;
        startActivityForResult(new Intent(this, FrontendStepActivity.class).putExtra("args", args), REQUEST_STEP);
    }

    /** A BuildService command this screen started has ended: the next step, or the error. */
    private void onServiceProgress() {
        boolean ended;
        int code;
        synchronized (BuildService.state) {
            ended = serviceGeneration >= 0 && BuildService.state.generation >= serviceGeneration && !BuildService.state.running;
            code = BuildService.state.exitCode;
        }
        if (ended) {
            serviceGeneration = -1;
            if (code == 0) {
                step = step == BUILD ? PICK : step + 1;
                if (step == PICK)
                    parts = new JSONArray(); // built: the dump isn't needed again
                save();
                if (step != PICK)
                    run();
            }
        } else if (serviceGeneration < 0 && step != PICK && !waitingForStep && !BuildService.state.running
            && BuildService.state.error == null && (step == FIRMWARE || step == CHECK || step == BUILD)) {
            // reopened while a service step had been under way (the app was closed): start it again
            run();
        }
        refresh();
    }

    private void play() {
        startActivity(new Intent(this, SetupActivity.class));
        finish();
    }

    private static byte[] readAll(InputStream in) throws java.io.IOException {
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        byte[] buffer = new byte[8192];
        for (int n; (n = in.read(buffer)) > 0;)
            out.write(buffer, 0, n);
        return out.toByteArray();
    }
}
