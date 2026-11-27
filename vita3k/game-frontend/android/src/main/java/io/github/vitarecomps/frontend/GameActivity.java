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

import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.hardware.input.InputManager;
import android.os.Build;
import android.os.Bundle;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;
import android.widget.RelativeLayout;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

/**
 * A game on the single-game frontend (ss-delta 09-android): SDL's activity running the game's
 * native library, lib&lt;game&gt;.so, named by the manifest's "game.library" meta-data. Its
 * SDL_main is the frontend's, which finds everything in the app's external files folder.
 *
 * The command line comes from the launching intent's "args" string array extra, so test tools
 * can start it as the desktop executables are started:
 *   am start -n &lt;package&gt;/io.github.vitarecomps.frontend.GameActivity --esa args --title-id,PCSA00152
 *
 * It also holds the on-screen controls (OnScreenControls), over SDL's surface once the game's frontend
 * configures them, and tells them when a gamepad is used or the last one goes.
 */
public class GameActivity extends SDLActivity implements InputManager.InputDeviceListener {
    private static final String TAG = "GameActivity";

    private OnScreenControls controls;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        hideSystemBars();
        InputManager input = (InputManager) getSystemService(Context.INPUT_SERVICE);
        if (input != null)
            input.registerInputDeviceListener(this, null);
    }

    @Override
    protected void onDestroy() {
        InputManager input = (InputManager) getSystemService(Context.INPUT_SERVICE);
        if (input != null)
            input.unregisterInputDeviceListener(this);
        super.onDestroy();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus)
            hideSystemBars();
        else if (controls != null)
            controls.releaseControls();
    }

    @Override
    protected void onPause() {
        if (controls != null)
            controls.releaseControls();
        super.onPause();
    }

    /**
     * The frontend's [controls] settings, the game's touch spots (960x544 points, x, y pairs) and its
     * latch button (a Vita button and a label; 0 and empty for none), from the game's thread
     * (game-frontend/android.cpp) when its window exists and when the settings menu changes them.
     */
    public void configureOnScreenControls(boolean enabled, int size, int opacity, float[] spots, int latchButton, String latchLabel) {
        runOnUiThread(() -> {
            if (controls == null) {
                if (mLayout == null)
                    return;
                controls = new OnScreenControls(this);
                mLayout.addView(controls, new RelativeLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
                // a handheld's built-in controls (or a gamepad already connected): not needed
                controls.setHiddenByGamepad(gamepadConnected());
            }
            controls.configure(enabled, size, opacity, spots, latchButton, latchLabel);
        });
    }

    /** An Android action selected from the native settings menu (game-frontend/android.cpp). */
    public void onMenuAction(String action) {
        if ("diagnostic".equals(action)) runOnUiThread(() -> ImportDiagnostics.share(this));
    }

    /** The frontend settings menu opened or closed. */
    public void onSettingsMenu(boolean open) {
        runOnUiThread(() -> {
            if (controls != null)
                controls.setSettingsMenuOpen(open);
        });
    }

    // a gamepad's buttons and sticks hide the on-screen controls
    @Override
    public boolean dispatchKeyEvent(KeyEvent event) {
        if (controls != null && event.getAction() == KeyEvent.ACTION_DOWN && fromGamepad(event.getDevice(), event.getSource()))
            controls.gamepadUsed();
        return super.dispatchKeyEvent(event);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent event) {
        if (controls != null && fromGamepad(event.getDevice(), event.getSource()) && stickMoved(event))
            controls.gamepadUsed();
        return super.dispatchGenericMotionEvent(event);
    }

    // a stick, trigger or hat pushed well away from rest (not a resting stick's noise)
    private static boolean stickMoved(MotionEvent event) {
        final int[] axes = { MotionEvent.AXIS_X, MotionEvent.AXIS_Y, MotionEvent.AXIS_Z, MotionEvent.AXIS_RZ,
            MotionEvent.AXIS_HAT_X, MotionEvent.AXIS_HAT_Y, MotionEvent.AXIS_LTRIGGER, MotionEvent.AXIS_RTRIGGER };
        for (int axis : axes)
            if (Math.abs(event.getAxisValue(axis)) > 0.5f)
                return true;
        return false;
    }

    // an event from a gamepad's buttons or sticks (a phone's own keys come from a keyboard source)
    private static boolean fromGamepad(InputDevice device, int source) {
        if (device == null || device.isVirtual())
            return false;
        return (source & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
            || (source & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    // a gamepad connected: buttons and sticks both (a phone's own keys and sensors are neither)
    private static boolean isGamepad(InputDevice device) {
        if (device == null || device.isVirtual())
            return false;
        int sources = device.getSources();
        return (sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
            && (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    private static boolean gamepadConnected() {
        for (int id : InputDevice.getDeviceIds())
            if (isGamepad(InputDevice.getDevice(id)))
                return true;
        return false;
    }

    @Override
    public void onInputDeviceAdded(int deviceId) {
    }

    @Override
    public void onInputDeviceChanged(int deviceId) {
    }

    @Override
    public void onInputDeviceRemoved(int deviceId) {
        if (controls != null && !gamepadConnected())
            controls.gamepadsGone();
    }

    // The whole screen from the start: the surface is created at its full size, so the renderer's
    // swapchain is too (a swipe from the edge shows the bars for a moment)
    private void hideSystemBars() {
        Window window = getWindow();
        WindowManager.LayoutParams attributes = window.getAttributes();
        attributes.layoutInDisplayCutoutMode = WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;
        window.setAttributes(attributes);
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
            window.setDecorFitsSystemWindows(false);
            WindowInsetsController controller = window.getInsetsController();
            if (controller != null) {
                controller.hide(WindowInsets.Type.systemBars());
                controller.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        } else {
            window.getDecorView().setSystemUiVisibility(View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION
                | View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY | View.SYSTEM_UI_FLAG_LAYOUT_STABLE
                | View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION | View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN);
        }
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { gameLibrary() };
    }

    @Override
    protected String[] getArguments() {
        Intent intent = getIntent();
        String[] args = intent != null ? intent.getStringArrayExtra("args") : null;
        return args != null ? args : new String[0];
    }

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return new GameSurface(context);
    }

    private String gameLibrary() {
        try {
            ActivityInfo info = getPackageManager().getActivityInfo(getComponentName(), PackageManager.GET_META_DATA);
            if (info.metaData != null) {
                String name = info.metaData.getString("game.library");
                if (name != null && !name.isEmpty())
                    return name;
            }
        } catch (PackageManager.NameNotFoundException e) {
            Log.e(TAG, "no activity info", e);
        }
        return "main";
    }
}
