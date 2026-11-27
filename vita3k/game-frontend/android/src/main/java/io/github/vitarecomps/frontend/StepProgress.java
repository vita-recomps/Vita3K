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
import android.view.*;
import android.widget.*;

import java.util.Arrays;

/** Setup retains its native SDL lifecycle; only installation windows are invisible. */
public final class StepProgress {
    private static boolean installing(Activity activity) {
        String[] args = activity.getIntent().getStringArrayExtra("args");
        return args != null
                && (Arrays.asList(args).contains("--install-content")
                        || Arrays.asList(args).contains("--install-firmware"));
    }

    public static void prepareWindow(Activity activity) {
        if (!installing(activity)) return;
        activity.setTheme(android.R.style.Theme_Translucent_NoTitleBar_Fullscreen);
        Window window = activity.getWindow();
        window.setBackgroundDrawable(
                new android.graphics.drawable.ColorDrawable(android.graphics.Color.TRANSPARENT));
        WindowManager.LayoutParams params = window.getAttributes();
        params.alpha = 0f;
        window.setAttributes(params);
    }

    public static void attach(Activity activity) {
        if (installing(activity)) return;
        String[] args = activity.getIntent().getStringArrayExtra("args");
        if (args == null || !Arrays.asList(args).contains("--dump-elfs")) return;
        View content = activity.findViewById(android.R.id.content);
        if (!(content instanceof ViewGroup)) return;
        TextView banner = new TextView(activity);
        banner.setText(
                "Collecting modules for on-device compilation. This temporary game run closes"
                    + " automatically, then setup continues.");
        banner.setTextSize(16);
        banner.setTextColor(0xffeeeeee);
        banner.setBackgroundColor(0xee10151c);
        banner.setPadding(24, 16, 24, 16);
        FrameLayout.LayoutParams params = new FrameLayout.LayoutParams(-1, -2);
        params.gravity = Gravity.TOP;
        ((ViewGroup) content).addView(banner, params);
    }

    public static void stop(Activity activity) {}
}
