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
import android.content.Context;
import android.graphics.Typeface;
import android.graphics.drawable.GradientDrawable;
import android.view.*;
import android.widget.*;

import java.io.File;
import java.lang.ref.WeakReference;
import java.util.*;

/** Reuses the installer's original views and listeners; changes presentation only. */
public final class ImportScreen {
    private static final int BACKGROUND = 0xff10151c, PANEL = 0xff18202c, BORDER = 0xff35404d;
    private static final int TEXT = 0xffeeeeee, MUTED = 0xffb6bcc4, READY = 0xffb8dfc3;
    private static final WeakHashMap<Activity, WeakReference<ImportScreen>> screens =
            new WeakHashMap<>();
    private final Activity activity;
    private TextView arm, firmware, fonts, installed, installationDetail;
    private ProgressBar installationProgress;
    private LinearLayout installationCard;
    private final List<Button> importButtons = new ArrayList<>();

    private ImportScreen(Activity activity) {
        this.activity = activity;
    }

    private int dp(int value) {
        return Math.round(value * activity.getResources().getDisplayMetrics().density);
    }

    private TextView text(String value, int size, boolean bold) {
        TextView view = new TextView(activity);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(bold ? TEXT : MUTED);
        if (bold) view.setTypeface(Typeface.DEFAULT_BOLD);
        return view;
    }

    private GradientDrawable background(int color) {
        GradientDrawable result = new GradientDrawable();
        result.setColor(color);
        result.setCornerRadius(dp(12));
        result.setStroke(dp(1), BORDER);
        return result;
    }

    private void buttonStyle(Button button) {
        boolean play = "Play".contentEquals(button.getText());
        if (play) button.setText("\u25b6 Play");
        button.setAllCaps(false);
        button.setTextSize(14);
        button.setTextColor(TEXT);
        button.setMinHeight(dp(44));
        button.setMinimumHeight(dp(44));
        button.setPadding(dp(14), dp(8), dp(14), dp(8));
        android.graphics.drawable.StateListDrawable states =
                new android.graphics.drawable.StateListDrawable();
        states.addState(new int[] {-android.R.attr.state_enabled}, background(0xff2b3038));
        states.addState(new int[] {}, background(play ? 0xff268f87 : 0xff494d53));
        button.setBackground(states);
        button.setTextColor(
                new android.content.res.ColorStateList(
                        new int[][] {new int[] {-android.R.attr.state_enabled}, new int[] {}},
                        new int[] {0xff737b86, TEXT}));
    }

    private Button button(String label, Runnable action) {
        Button button = new Button(activity);
        button.setText(label);
        buttonStyle(button);
        button.setOnClickListener(v -> action.run());
        importButtons.add(button);
        return button;
    }

    private LinearLayout card(LinearLayout parent, String title, String description) {
        LinearLayout box = new LinearLayout(activity);
        box.setOrientation(LinearLayout.VERTICAL);
        box.setPadding(dp(16), dp(14), dp(16), dp(14));
        box.setBackground(background(PANEL));
        LinearLayout.LayoutParams params = new LinearLayout.LayoutParams(-1, -2);
        params.bottomMargin = dp(12);
        parent.addView(box, params);
        box.addView(text(title, 18, true));
        if (!description.isEmpty()) {
            TextView subtitle = text(description, 13, false);
            subtitle.setPadding(0, dp(5), 0, dp(10));
            box.addView(subtitle);
        }
        return box;
    }

    private TextView status(LinearLayout parent) {
        TextView view = text("", 14, false);
        view.setPadding(0, dp(8), 0, dp(8));
        parent.addView(view);
        return view;
    }

    private void actionRow(LinearLayout parent, Button... buttons) {
        WrapRow row = new WrapRow(activity, dp(8));
        for (Button button : buttons) row.addView(button);
        parent.addView(row, new LinearLayout.LayoutParams(-1, -2));
    }

    private void wrapOriginalButtons(LinearLayout row) {
        List<View> children = new ArrayList<>();
        for (int i = 0; i < row.getChildCount(); i++) children.add(row.getChildAt(i));
        row.removeAllViews();
        row.setOrientation(LinearLayout.VERTICAL);
        WrapRow wrap = new WrapRow(activity, dp(8));
        for (View view : children) {
            if (view instanceof Button) buttonStyle((Button) view);
            wrap.addView(view);
        }
        row.addView(wrap, new LinearLayout.LayoutParams(-1, -2));
    }

    public static void attach(
            BuildActivity activity,
            LinearLayout root,
            TextView title,
            LinearLayout fileButtons,
            TextView fileNames,
            LinearLayout actions,
            TextView step,
            TextView error,
            View logs) {
        ImportScreen screen = new ImportScreen(activity);
        screen.layout(root, title, fileButtons, fileNames, actions, step, error, logs);
        screens.put(activity, new WeakReference<>(screen));
        root.setTag(screen);
        screen.update();
    }

    private void layout(
            LinearLayout root,
            TextView title,
            LinearLayout fileButtons,
            TextView fileNames,
            LinearLayout actions,
            TextView step,
            TextView error,
            View logs) {
        LinearLayout diagnostics = new LinearLayout(activity);
        diagnostics.setOrientation(LinearLayout.HORIZONTAL);
        actionRow(
                diagnostics,
                button("View report", () -> ImportDiagnostics.show(activity)),
                button("Share report", () -> ImportDiagnostics.share(activity)));
        root.removeAllViews();
        root.setPadding(dp(20), dp(18), dp(20), dp(20));
        root.setBackgroundColor(BACKGROUND);
        title.setText(title.getText().toString().replaceFirst("^Set up ", ""));
        title.setTextSize(27);
        title.setTextColor(TEXT);
        title.setTypeface(Typeface.DEFAULT_BOLD);
        root.addView(title);
        TextView subtitle = text("Set up your game", 15, false);
        subtitle.setPadding(0, dp(4), 0, dp(18));
        root.addView(subtitle);
        TextView supported =
                text(
                        "Only Soul Sacrifice Delta version 1.30 is supported. Add the 1.30 update"
                            + " before building.",
                        14,
                        true);
        supported.setPadding(0, 0, 0, dp(14));
        root.addView(supported);
        LinearLayout game =
                card(
                        root,
                        "Game & updates",
                        "Add your game, its update and the matching licence. Clear removes a"
                            + " mistaken selection before you build.");
        wrapOriginalButtons(fileButtons);
        game.addView(fileButtons);
        fileNames.setTextColor(MUTED);
        fileNames.setTextSize(14);
        fileNames.setPadding(0, dp(10), 0, dp(4));
        game.addView(fileNames);
        installed = status(game);
        LinearLayout system =
                card(
                        root,
                        "Firmware & fonts",
                        "Import your PUP files for offline installation. If none are imported,"
                            + " setup uses the original download method.");
        firmware = status(system);
        fonts = status(system);
        if (activity instanceof BuildActivity) {
            BuildActivity setup = (BuildActivity) activity;
            Button importPup = setup.officialFirmwareButton();
            TextView officialStatus = setup.officialFirmwareStatus();
            if (importPup.getParent() instanceof ViewGroup)
                ((ViewGroup) importPup.getParent()).removeView(importPup);
            if (officialStatus.getParent() instanceof ViewGroup)
                ((ViewGroup) officialStatus.getParent()).removeView(officialStatus);
            buttonStyle(importPup);
            importButtons.add(importPup);
            actionRow(system, importPup);
            // The official status came from a horizontal row (width 0, weight 1).
            // In this vertical card it must wrap its content without vertical weight.
            LinearLayout.LayoutParams statusParams =
                    new LinearLayout.LayoutParams(
                            ViewGroup.LayoutParams.MATCH_PARENT,
                            ViewGroup.LayoutParams.WRAP_CONTENT);
            statusParams.weight = 0f;
            officialStatus.setLayoutParams(statusParams);
            officialStatus.setTextSize(13);
            officialStatus.setTextColor(MUTED);
            officialStatus.setPadding(0, dp(8), 0, 0);
            system.addView(officialStatus);
        }
        LinearLayout modules =
                card(
                        root,
                        "ARM64 modules",
                        "Already compiled the modules? Import the folder or ZIP containing the .so"
                            + " files. The manifest is created automatically.");
        arm = status(modules);
        actionRow(
                modules,
                button("Import folder", () -> OfflineImports.choose(activity, 1)),
                button("Import ZIP", () -> OfflineImports.choose(activity, 0)));
        TextView warning =
                text(
                        "Without imported ARM64 modules, Build compiles the code on this device."
                            + " This can take a long time and require several GB of free space.",
                        12,
                        false);
        warning.setPadding(0, dp(12), 0, 0);
        modules.addView(warning);
        LinearLayout installation =
                card(root, "Installation", "When your files are ready, select Build to continue.");
        installationCard = installation;
        wrapOriginalButtons(actions);
        installation.addView(actions);
        step.setTextSize(16);
        step.setTextColor(TEXT);
        installation.addView(step);
        installationDetail = text("", 14, false);
        installationDetail.setVisibility(View.GONE);
        installation.addView(installationDetail);
        installationProgress =
                new ProgressBar(activity, null, android.R.attr.progressBarStyleHorizontal);
        installationProgress.setMax(100);
        installationProgress.setVisibility(View.GONE);
        installation.addView(installationProgress, new LinearLayout.LayoutParams(-1, dp(24)));
        installation.addView(error);
        Button details = button("Show diagnostics", () -> {});
        root.addView(details, new LinearLayout.LayoutParams(-1, -2));
        if (diagnostics != null) {
            wrapOriginalButtons(diagnostics);
            for (int i = 0; i < ((ViewGroup) diagnostics.getChildAt(0)).getChildCount(); i++) {
                View view = ((ViewGroup) diagnostics.getChildAt(0)).getChildAt(i);
                if (view instanceof Button)
                    ((Button) view).setText(i == 0 ? "View report" : "Share report");
            }
            root.addView(diagnostics);
            diagnostics.setVisibility(View.GONE);
        }
        root.addView(logs);
        logs.setVisibility(View.GONE);
        final LinearLayout reportActions = diagnostics;
        details.setOnClickListener(
                v -> {
                    boolean show = logs.getVisibility() != View.VISIBLE;
                    logs.setVisibility(show ? View.VISIBLE : View.GONE);
                    if (reportActions != null)
                        reportActions.setVisibility(show ? View.VISIBLE : View.GONE);
                    details.setText(show ? "Hide diagnostics" : "Show diagnostics");
                });
    }

    public static void refresh(Activity activity) {
        WeakReference<ImportScreen> ref = screens.get(activity);
        ImportScreen screen = ref == null ? null : ref.get();
        if (screen != null) screen.update();
    }

    public static void setupState(Activity activity, boolean busy, boolean running, String error) {
        setBusy(activity, busy && (running || error == null));
    }

    public static void setBusy(Activity activity, boolean busy) {
        WeakReference<ImportScreen> ref = screens.get(activity);
        ImportScreen screen = ref == null ? null : ref.get();
        if (screen == null) return;
        for (Button button : screen.importButtons) button.setEnabled(!busy);
    }

    public static void progress(Activity activity, boolean visible, int percent, String message) {
        WeakReference<ImportScreen> ref = screens.get(activity);
        ImportScreen screen = ref == null ? null : ref.get();
        if (screen == null) return;
        boolean wasVisible = screen.installationProgress.getVisibility() == View.VISIBLE;
        screen.installationDetail.setText(message);
        screen.installationDetail.setVisibility(visible ? View.VISIBLE : View.GONE);
        screen.installationProgress.setVisibility(visible ? View.VISIBLE : View.GONE);
        screen.installationProgress.setIndeterminate(percent < 0);
        if (percent >= 0) screen.installationProgress.setProgress(percent);
        if (visible && !wasVisible)
            screen.installationCard.post(
                    () ->
                            screen.installationCard.requestRectangleOnScreen(
                                    new android.graphics.Rect(
                                            0,
                                            0,
                                            screen.installationCard.getWidth(),
                                            screen.installationCard.getHeight()),
                                    false));
    }

    private void row(TextView row, boolean ready, String name, String missing) {
        row.setText(
                (ready ? "\u2713 " : "\u25cb ")
                        + name
                        + (ready ? "  \u00b7  Verified" : "  \u00b7  " + missing));
        row.setTextColor(ready ? READY : MUTED);
    }

    private void update() {
        File files = activity.getFilesDir(), pup = new File(files, "setup-work/firmware");
        row(arm, new File(files, "arm64-import.zip").isFile(), "ARM64 modules", "Not imported");
        row(firmware, new File(pup, "PSVUPDAT.PUP").isFile(), "Firmware", "Not imported");
        row(fonts, new File(pup, "PSP2UPDAT.PUP").isFile(), "Fonts", "Not imported");
        boolean done =
                activity.getSharedPreferences("offline-import-status", 0)
                        .getBoolean("pkg_installed", false);
        installed.setText(
                done
                        ? "\u2713 Game files and licence installed successfully"
                        : "Files are checked during installation.");
        installed.setTextColor(done ? READY : MUTED);
    }

    /** Buttons wrap at the available width; keeps existing listeners, focus and visibility. */
    private static final class WrapRow extends ViewGroup {
        private final int gap;

        WrapRow(Context context, int gap) {
            super(context);
            this.gap = gap;
        }

        protected void onMeasure(int widthSpec, int heightSpec) {
            int width = MeasureSpec.getSize(widthSpec), x = 0, y = 0, line = 0;
            for (int i = 0; i < getChildCount(); i++) {
                View child = getChildAt(i);
                if (child.getVisibility() == GONE) continue;
                child.measure(
                        MeasureSpec.makeMeasureSpec(width, MeasureSpec.AT_MOST),
                        MeasureSpec.makeMeasureSpec(0, MeasureSpec.UNSPECIFIED));
                int w = child.getMeasuredWidth(), h = child.getMeasuredHeight();
                if (x > 0 && x + w > width) {
                    x = 0;
                    y += line + gap;
                    line = 0;
                }
                x += w + gap;
                line = Math.max(line, h);
            }
            setMeasuredDimension(resolveSize(width, widthSpec), resolveSize(y + line, heightSpec));
        }

        protected void onLayout(boolean changed, int left, int top, int right, int bottom) {
            int width = right - left, x = 0, y = 0, line = 0;
            for (int i = 0; i < getChildCount(); i++) {
                View child = getChildAt(i);
                if (child.getVisibility() == GONE) continue;
                int w = child.getMeasuredWidth(), h = child.getMeasuredHeight();
                if (x > 0 && x + w > width) {
                    x = 0;
                    y += line + gap;
                    line = 0;
                }
                child.layout(x, y, x + w, y + h);
                x += w + gap;
                line = Math.max(line, h);
            }
        }
    }
}
