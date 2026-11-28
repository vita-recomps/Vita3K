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
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.os.SystemClock;

import android.view.DisplayCutout;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;

import org.libsdl.app.SDLActivity;

import java.util.ArrayList;
import java.util.List;

/**
 * On-screen controls over the game (ss-delta 09-android, "On-screen controls"), for phones without a
 * gamepad: the Vita's buttons, D-pad and both sticks, plus a button for the frontend's settings menu.
 * What they hold goes to the game through game-frontend/android.cpp (CtrlState::screen_state).
 *
 * The game's own touchscreen keeps working. This view lies over SDL's surface and takes every finger:
 * one that lands on a control is the control's until it lifts; any other is handed to SDL as a touch,
 * exactly as SDLSurface would have (Android gives a view that already holds a finger every further
 * finger inside it, so passing a touch on by declining it would fail while a thumb is on a stick). The
 * game's touch spots (its [touch] bindings' points, such as Soul Sacrifice's Black Rite icon) are kept
 * clear: controls are moved off them, and a finger on one always goes to the game.
 *
 * They hide while a gamepad is used (or is connected when the game starts: a handheld's built-in
 * controls) and come back when the last gamepad is disconnected or the setting is switched on again.
 * A game may ask for a latch button (Profile::screen_latch; ss-delta: LOCK, L for the lock-on): a tap
 * holds its Vita button down until the next tap.
 * Switched off, only the menu button stays, and they all show while the settings menu is open: a phone
 * without a gamepad can always reach the setting again.
 */
public class OnScreenControls extends View {
    // SceCtrlButtons (vita3k/ctrl/include/ctrl/ctrl.h) as the extended functions read them
    static final int SELECT = 0x1, START = 0x8, UP = 0x10, RIGHT = 0x20, DOWN = 0x40, LEFT = 0x80,
        L1 = 0x400, R1 = 0x800, TRIANGLE = 0x1000, CIRCLE = 0x2000, CROSS = 0x4000, SQUARE = 0x8000;

    // what a finger belongs to
    private static final int OWNER_GAME = 0, OWNER_BUTTONS = 1, OWNER_DPAD = 2, OWNER_LEFT_STICK = 3,
        OWNER_RIGHT_STICK = 4, OWNER_MENU = 5, OWNER_NONE = 6, // none: ignored until it lifts
        OWNER_LATCH = 7;

    // A press reaches the game for at least this long: the game reads the buttons once a frame, and a
    // quick tap could otherwise start and end between two reads (adb's taps did)
    private static final long MIN_PRESS_MS = 70;

    // a game touch spot's radius in the Vita's 960x544 points (**hypothesis**: the Black Rite icon's size)
    private static final float SPOT_RADIUS = 36f;

    private static native void setState(int buttons, float lx, float ly, float rx, float ry);
    private static native void toggleMenu();

    private static final int SYMBOL_TEXT = 0, SYMBOL_TRIANGLE = 1, SYMBOL_CIRCLE = 2, SYMBOL_CROSS = 3,
        SYMBOL_SQUARE = 4, SYMBOL_MENU = 5;

    private static final class Button {
        final int bit; // 0: the menu button
        final int symbol;
        final String label;
        final int color;
        final boolean round;
        int customShape;
        boolean extra,hidden,touchButton,touchHeld,touchSent,toggle,toggled;
        float targetX=.5f,targetY=.5f;
        int syntheticId;
        float labelWidth=-1,labelHeight=-1,labelSize;
        ControlElement element;
        int sourceSlot = -1;
        final RectF rect = new RectF();
        boolean pressed;

        Button(int bit, int symbol, String label, int color, boolean round) {
            this.bit = bit;
            this.symbol = symbol;
            this.label = label;
            this.color = color;
            this.round = round;
            this.element = new ControlElement(bit == 0 ? "menu" : "button_" + bit, bit == 0 ? ControlElement.Type.MENU : ControlElement.Type.BUTTON, bit == 0 ? 1000 : 800, bit);
        }
    }

    private final class Stick {
 org.json.JSONObject config=new org.json.JSONObject(); final FloatingAnalog floating=new FloatingAnalog();
 int modeValue,curveValue,restrictionValue;
 boolean availableValue=true,followValue,invertXValue,invertYValue,idleValue,baseValue=true,knobValue=true;
 float marginValue=24,activationValue=1.5f,radiusValue=60,sxValue=1,syValue=1,deadValue=.08f,maximumValue=1,smoothingValue;
 boolean available(){return availableValue;}
 int mode(){return modeValue;}float option(String k,float fallback){return (float)config.optDouble(k,fallback);}
 void refresh(){
  modeValue=config.optInt("mode",0);availableValue=config.optBoolean("enabled",true);
  marginValue=option("margin",24);activationValue=option("activationScale",1.5f);radiusValue=Math.max(20,Math.min(160,option("radius",60)));
  sxValue=option("sx",1);syValue=option("sy",1);deadValue=option("dead",.08f);maximumValue=option("maximum",1);smoothingValue=option("smoothing",0);
  curveValue=config.optInt("curve",0);restrictionValue=config.optInt("restrict",0);followValue=config.optBoolean("follow",false);invertXValue=config.optBoolean("invertX",false);invertYValue=config.optBoolean("invertY",false);
  idleValue=config.optBoolean("idle",false);baseValue=config.optBoolean("base",true);knobValue=config.optBoolean("knob",true);
 }
 boolean region(float x,float y){return AnalogActivation.contains(x,y,homeX,homeY,radius,marginValue,getResources().getDisplayMetrics().density,activationValue);}
 boolean tick(){if(pointer<0||mode()==0)return false;boolean pending=floating.tick(SystemClock.uptimeMillis(),smoothingValue);x=floating.x;y=floating.y;centerX=floating.baseX;centerY=floating.baseY;return pending;}

        private final AnalogControl analog = new AnalogControl();
        float homeX, homeY, radius; // where it rests, the base's radius
        float centerX, centerY; // while held: where the finger came down
        float x, y; // -1 to 1, right and down positive
        int pointer = -1;

        void press(int id, float px, float py) {
            pointer = id;
            centerX = px;
            centerY = py;
            x = y = 0;
 if(mode()!=0){floating.press(mode()==1?homeX:px,mode()==1?homeY:py,SystemClock.uptimeMillis());move(px,py);}
        }

        void move(float px, float py) {
            if(mode()!=0){floating.move(px,py,radiusValue*getResources().getDisplayMetrics().density,mode()==2&&followValue,sxValue,syValue,deadValue,maximumValue,curveValue,restrictionValue,invertXValue,invertYValue);if(tick())scheduleAnalog();return;}
 analog.move(px-centerX,py-centerY,radius);
            x=analog.x; y=analog.y;
        }

        void release() {
            analog.release();floating.release();
            pointer = -1;
            x = y = 0;
        }
    }

    private final List<Button> buttons = new ArrayList<>();
    private final Button menu, l, r, select, start;
    private Button latch; // the game's latch button, or null
    private boolean latched; // its Vita button is held
    private final RectF dpad = new RectF();
    private final RectF dpadOuter=new RectF(),dpadHub=new RectF();
    private static final int[] DPAD_BITS={UP,RIGHT,DOWN,LEFT};
    private int dpadBits;
    private final Stick leftStick = new Stick(), rightStick = new Stick();
    private final InputStateAggregator inputs = new InputStateAggregator(64);
    private final ControlElement dpadElement = new ControlElement("dpad", ControlElement.Type.DPAD, 700, 0);
    private final ControlElement leftElement = new ControlElement("left_stick", ControlElement.Type.LEFT_ANALOG, 600, 0);
    private final ControlElement rightElement = new ControlElement("right_stick", ControlElement.Type.RIGHT_ANALOG, 600, 0);
    private final int dpadSource = inputs.register(dpadElement), leftSource = inputs.register(leftElement), rightSource = inputs.register(rightElement);
    private final TouchInputManager owners = new TouchInputManager(32);

    private boolean enabled; // [controls] on_screen
    private boolean configured; // configure has been called (the game's first call isn't a switch)
    private boolean hiddenByGamepad;
    private boolean settingsMenuOpen;
    private int sizePercent = 100;
    private float[] spotPoints = new float[0]; // 960x544 points, x, y pairs
    private float[] spots = new float[0]; // on the screen: x, y, radius triples
    private float unit = 1; // a dp at the chosen size

    private int sentButtons;
    private int heldButtons; // what the fingers hold now (sentButtons may keep a short press longer)
    private final long[] pressedAt = new long[16]; // when each button bit last went down
    private final Runnable resend = this::send;
 private boolean analogScheduled;
 private final Runnable analogFrame = () -> {analogScheduled=false;float lx=leftStick.x,ly=leftStick.y,rx=rightStick.x,ry=rightStick.y;boolean pending=leftStick.tick() | rightStick.tick();if(lx!=leftStick.x||ly!=leftStick.y||rx!=rightStick.x||ry!=rightStick.y){send();invalidate();}if(pending)scheduleAnalog();};
 private void scheduleAnalog(){if(!analogScheduled&&!editingLayout){analogScheduled=true;postOnAnimation(analogFrame);}}

    private float sentLx, sentLy, sentRx, sentRy;

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint stroke = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint symbol = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Path path = new Path();
    private final RectF block = new RectF();

    public OnScreenControls(Context context) {
        super(context);
        reloadCustomLayout();
        setFocusable(false); // gamepad events stay with SDL's surface
        stroke.setStyle(Paint.Style.STROKE);
        symbol.setStyle(Paint.Style.STROKE);
        symbol.setStrokeCap(Paint.Cap.ROUND);
        symbol.setStrokeJoin(Paint.Join.ROUND);
        text.setColor(Color.WHITE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setTypeface(Typeface.create(Typeface.DEFAULT, Typeface.BOLD));

        // the PlayStation colours of the face buttons' symbols
        buttons.add(new Button(TRIANGLE, SYMBOL_TRIANGLE, null, Color.WHITE, true));
        buttons.add(new Button(CIRCLE, SYMBOL_CIRCLE, null, Color.WHITE, true));
        buttons.add(new Button(CROSS, SYMBOL_CROSS, null, Color.WHITE, true));
        buttons.add(new Button(SQUARE, SYMBOL_SQUARE, null, Color.WHITE, true));
        buttons.add(l = new Button(L1, SYMBOL_TEXT, "L", Color.WHITE, false));
        buttons.add(r = new Button(R1, SYMBOL_TEXT, "R", Color.WHITE, false));
        buttons.add(select = new Button(SELECT, SYMBOL_TEXT, "SELECT", Color.WHITE, false));
        buttons.add(start = new Button(START, SYMBOL_TEXT, "START", Color.WHITE, false));
        buttons.add(menu = new Button(0, SYMBOL_MENU, null, Color.WHITE, true));
        for (Button b : buttons) b.sourceSlot = inputs.register(b.element);
        reloadExtraButtons();
    }

    /** [controls] from the game (any thread's call is posted here by GameActivity). */
    public void configure(boolean on, int size, int opacity, float[] points, int latchButton, String latchLabel) {
        if (latchButton == 0 || latchLabel == null || latchLabel.isEmpty())
            latch = null;
        else if (latch == null || latch.bit != latchButton || !latch.label.equals(latchLabel))
        {
            latch = new Button(latchButton, 11, latchLabel, Color.WHITE, true);
            latch.element = new ControlElement("lock", ControlElement.Type.BUTTON, 800, latchButton);
            latch.sourceSlot = inputs.register(latch.element);
        }
        if (latch == null)
            latched = false;
        if (on && !enabled && configured)
            hiddenByGamepad = false; // switched on in the settings menu: shown, gamepad or not
        configured = true;
        enabled = on;
        sizePercent = size;
        spotPoints = points != null ? points : new float[0];
        setAlpha(Math.max(0.1f, Math.min(1f, opacity / 100f)));
        if (!controlsShown())
            releaseControls();
        layoutControls();
        invalidate();
    }

    /** The frontend's settings menu opened or closed. */
    public void setSettingsMenuOpen(boolean open) {
        if (open && !settingsMenuOpen) releaseControls();
        settingsMenuOpen = open;
        if (!controlsShown())
            releaseControls();
        else if (open && latched) {
            // the menu pages with L and R; a latched shoulder button would keep its press from registering
            latched = false;
            latch.pressed = false;
            send();
        }
        invalidate();
    }

    /** At the first configure: a gamepad is connected (a handheld's built-in controls). */
    public void setHiddenByGamepad(boolean hidden) {
        hiddenByGamepad = hidden;
        if (hidden)
            releaseControls();
        invalidate();
    }

    /** A gamepad's button or stick was used. */
    public void gamepadUsed() {
        if (!hiddenByGamepad)
            setHiddenByGamepad(true);
    }

    /** The last gamepad is gone. */
    public void gamepadsGone() {
        if (hiddenByGamepad)
            setHiddenByGamepad(false);
    }

    // the menu button: unless a gamepad is in use
    private boolean menuButtonShown() {
        return !hiddenByGamepad;
    }

    // everything else: switched on, or while the settings menu is open
    private boolean controlsShown() {
        return !hiddenByGamepad && (enabled || settingsMenuOpen);
    }

    /** Everything held is let go (the app leaves the screen, the controls hide). */
    public void releaseControls() {
        syncTouchButtons(true);
        for (Button b : buttons){b.pressed = false;b.toggled=false;}
        latched = false;
        if (latch != null)
            latch.pressed = false;
        dpadBits = 0;
        leftStick.release();
        rightStick.release();
        // Cancel only virtual touch sources; physical controller state is owned by the runtime.
        for (int i = 0; i < owners.size(); i++) {
            if (owners.valueAt(i) == OWNER_GAME)
                SDLActivity.onNativeTouch(owners.deviceAt(i), owners.keyAt(i), MotionEvent.ACTION_CANCEL, owners.xAt(i), owners.yAt(i), 0);
            owners.setValueAt(i, OWNER_NONE);
        }
        clearPendingVirtualInput();
        invalidate();
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        super.onSizeChanged(w, h, oldw, oldh);
        layoutControls();
    }

    @Override
    public WindowInsets onApplyWindowInsets(WindowInsets insets) {
        WindowInsets result = super.onApplyWindowInsets(insets);
        layoutControls();
        return result;
    }

    // Phone landscape: L above the D-pad above the left stick on the left, R above the face buttons
    // above the right stick on the right, SELECT and START at the bottom's middle, the menu button at
    // the top's. Sizes in dp, shrunk to fit a short screen.
    private void layoutControls() {
        int w = getWidth(), h = getHeight();
        if (w == 0 || h == 0)
            return;
        float density = getResources().getDisplayMetrics().density;
        float u = Math.min(density * sizePercent / 100f, h / 360f);
        unit = u;
        float insetLeft = 0, insetRight = 0;
        WindowInsets insets = getRootWindowInsets();
        DisplayCutout cutout = insets != null ? insets.getDisplayCutout() : null;
        if (cutout != null) {
            // the picture reaches under a camera hole (GameActivity's cutout mode); the controls don't
            insetLeft = cutout.getSafeInsetLeft();
            insetRight = cutout.getSafeInsetRight();
        }
        float margin = 14 * u;
        float left = insetLeft + margin, right = w - insetRight - margin, top = margin, bottom = h - margin;

        l.rect.set(left, top, left + 96 * u, top + 42 * u);
        r.rect.set(right - 96 * u, top, right, top + 42 * u);
        // the latch beside the shoulder button it holds, smaller (ss-delta's lock-on goes with L,
        // Shinobido 2's crouch with R); beside L for any other button
        if (latch != null && latch.bit == R1)
            latch.rect.set(r.rect.left - 88 * u, top + 4 * u, r.rect.left - 12 * u, top + 38 * u);
        else if (latch != null)
            latch.rect.set(l.rect.right + 12 * u, top + 4 * u, l.rect.right + 88 * u, top + 38 * u);

        // the game's touch spots on the screen, where Vita3K draws the picture: 960x544 fitted in the middle
        float scale = Math.min(w / 960f, h / 544f);
        float originX = (w - 960 * scale) / 2, originY = (h - 544 * scale) / 2;
        spots = new float[spotPoints.length / 2 * 3];
        for (int i = 0, j = 0; i + 1 < spotPoints.length; i += 2, j += 3) {
            spots[j] = originX + spotPoints[i] * scale;
            spots[j + 1] = originY + spotPoints[i + 1] * scale;
            spots[j + 2] = SPOT_RADIUS * scale;
        }
        float gap = 6 * u;

        // the sticks in the lower corners, moved straight up (or down) off any spot they cover, so the
        // D-pad and face buttons above them stay in line (Soul Sacrifice's Black Rite icon, lower left)
        float stickRadius = 58 * u;
        leftStick.radius = rightStick.radius = stickRadius;
        leftStick.homeX = left + 70 * u;
        leftStick.homeY = clearVertically(leftStick.homeX, bottom - 70 * u, stickRadius + gap);
        rightStick.homeX = right - 70 * u;
        rightStick.homeY = clearVertically(rightStick.homeX, bottom - 70 * u, stickRadius + gap);

        float dpadHalf = 66 * u;
        float dpadX = left + 70 * u, dpadY = leftStick.homeY - stickRadius - 16 * u - dpadHalf;
        dpad.set(dpadX - dpadHalf, dpadY - dpadHalf, dpadX + dpadHalf, dpadY + dpadHalf);

        float faceRadius = 26 * u, faceOffset = 48 * u;
        float faceX = right - 80 * u, faceY = rightStick.homeY - stickRadius - 12 * u - faceOffset - faceRadius;
        setCircle(buttons.get(0), faceX, faceY - faceOffset, faceRadius); // triangle
        setCircle(buttons.get(1), faceX + faceOffset, faceY, faceRadius); // circle
        setCircle(buttons.get(2), faceX, faceY + faceOffset, faceRadius); // cross
        setCircle(buttons.get(3), faceX - faceOffset, faceY, faceRadius); // square

        float middle = w / 2f;
        select.rect.set(middle - 96 * u, bottom - 30 * u, middle - 16 * u, bottom);
        start.rect.set(middle + 16 * u, bottom - 30 * u, middle + 96 * u, bottom);
        setCircle(menu, middle, top + 20 * u, 20 * u);
        keepSpotsClear(w, h, margin);
        applyCustomLayout();
    }

    // a stick's y, moved straight away from each spot whose circle its own (radius) would overlap
    private float clearVertically(float x, float y, float radius) {
        for (int s = 0; s + 2 < spots.length; s += 3) {
            float dx = Math.abs(x - spots[s]), reach = radius + spots[s + 2];
            if (dx >= reach)
                continue;
            float dy = (float) Math.sqrt(reach * reach - dx * dx);
            if (Math.abs(y - spots[s + 1]) < dy)
                y = y <= spots[s + 1] ? spots[s + 1] - dy : spots[s + 1] + dy;
        }
        return y;
    }

    private static void setCircle(Button b, float x, float y, float radius) {
        b.rect.set(x - radius, y - radius, x + radius, y + radius);
    }

    // Each other control that covers a touch spot moves straight away from it until it no longer does,
    // staying on the screen (the sticks were placed so already).
    private void keepSpotsClear(int w, int h, float margin) {
        float gap = 6 * unit;
        for (int pass = 0; pass < 3; pass++) {
            for (int s = 0; s + 2 < spots.length; s += 3) {
                float sx = spots[s], sy = spots[s + 1], sr = spots[s + 2];
                float half = dpad.width() / 2;
                float[] c = pushAway(dpad.centerX(), dpad.centerY(), half * 1.2f, sx, sy, sr + gap);
                float dx = clamp(c[0], margin + half, w - margin - half), dy = clamp(c[1], margin + half, h - margin - half);
                dpad.set(dx - half, dy - half, dx + half, dy + half);
                List<Button> all = new ArrayList<>(buttons);
                if (latch != null)
                    all.add(latch);
                for (Button b : all) {
                    float bw = b.rect.width() / 2, bh = b.rect.height() / 2;
                    c = pushAway(b.rect.centerX(), b.rect.centerY(), Math.max(bw, bh), sx, sy, sr + gap);
                    float bx = clamp(c[0], margin + bw, w - margin - bw), by = clamp(c[1], margin + bh, h - margin - bh);
                    b.rect.set(bx - bw, by - bh, bx + bw, by + bh);
                }
            }
        }
    }

    private static float[] pushAway(float x, float y, float radius, float sx, float sy, float sr) {
        float dx = x - sx, dy = y - sy;
        float distance = (float) Math.hypot(dx, dy);
        float need = radius + sr - distance;
        if (need <= 0)
            return new float[] { x, y };
        if (distance < 0.001f) {
            dx = 0;
            dy = -1;
            distance = 1;
        }
        return new float[] { x + dx / distance * need, y + dy / distance * need };
    }

    private static float clamp(float v, float lo, float hi) {
        return lo > hi ? (lo + hi) / 2 : Math.max(lo, Math.min(hi, v));
    }

    private boolean onSpot(float x, float y) {
        for (int s = 0; s + 2 < spots.length; s += 3)
            if (Math.hypot(x - spots[s], y - spots[s + 1]) <= spots[s + 2])
                return true;
        return false;
    }

    // what a finger coming down at x, y belongs to
    private int claim(float x, float y) {
        if (!menuButtonShown() || onSpot(x, y))
            return OWNER_GAME;
        float slack = 8 * unit;
        if (contains(menu.rect, x, y, slack))
            return OWNER_MENU;
        if (!controlsShown())
            return OWNER_GAME;
        if (latch != null && !latch.hidden && contains(latch.rect, x, y, slack))
            return OWNER_LATCH;
        if (buttonAt(x, y, slack) != null)
            return OWNER_BUTTONS;
        if (contains(dpad, x, y, slack))
            return OWNER_DPAD;
        // a stick's area is larger than its base; a second finger there while it is held does nothing
        if (leftStick.available() && (leftStick.mode()!=2 || settingsMenuOpen) && Math.hypot(x - leftStick.homeX, y - leftStick.homeY) <= leftStick.radius * 1.5f)
            return leftStick.pointer < 0 ? OWNER_LEFT_STICK : OWNER_NONE;
        if (rightStick.available() && (rightStick.mode()!=2 || settingsMenuOpen) && Math.hypot(x - rightStick.homeX, y - rightStick.homeY) <= rightStick.radius * 1.5f)
            return rightStick.pointer < 0 ? OWNER_RIGHT_STICK : OWNER_NONE;
 if(!settingsMenuOpen){boolean a=leftStick.available()&&leftStick.mode()==2&&leftStick.region(x,y),b=rightStick.available()&&rightStick.mode()==2&&rightStick.region(x,y);if(a&&leftStick.pointer<0)return OWNER_LEFT_STICK;if(b&&rightStick.pointer<0)return OWNER_RIGHT_STICK;if(a||b)return OWNER_NONE;}
        return OWNER_GAME;
    }

    private static boolean contains(RectF rect, float x, float y, float slack) {
        return x >= rect.left - slack && x <= rect.right + slack && y >= rect.top - slack && y <= rect.bottom + slack;
    }

    private Button buttonAt(float x, float y, float slack) {
        for (Button b : buttons) {
            if (b == menu || b.hidden)
                continue;
            if (b==latch || b.customShape == 1 || (b.customShape == 0 && b.round)) {
                if (Math.hypot(x - b.rect.centerX(), y - b.rect.centerY()) <= b.rect.width() / 2 + slack)
                    return b;
            } else if (contains(b.rect, x, y, slack))
                return b;
        }
        return null;
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (editingLayout) {if(pickingTarget)return targetTouch(event);return editTouch(event);}
        int action = event.getActionMasked();
        rememberPointers(event);
        if (action == MotionEvent.ACTION_DOWN)
            owners.clear();
        // hidden, and no finger of ours down: SDL's surface below takes the touches itself
        if (!menuButtonShown() && owners.size() == 0 && action == MotionEvent.ACTION_DOWN)
            return false;

        int index = event.getActionIndex();
        switch (action) {
        case MotionEvent.ACTION_DOWN:
        case MotionEvent.ACTION_POINTER_DOWN: {
            int id = event.getPointerId(index);
            float x = event.getX(index), y = event.getY(index);
            int owner = claim(x, y);
            owners.put(id, owner);
            owners.remember(id,event.getDeviceId(),x/Math.max(1,getWidth()-1),y/Math.max(1,getHeight()-1));
            if (owner == OWNER_GAME)
                forward(event, index, MotionEvent.ACTION_DOWN);
            else if (owner == OWNER_MENU) {
                menu.pressed = true;
                performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
                toggleMenu();
            } else if (owner == OWNER_LATCH) {
                latched = !latched;
                latch.pressed = latched;
                performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
            } else if(owner==OWNER_BUTTONS){Button hit=buttonAt(x,y,8*unit);if(hit!=null&&hit.toggle){hit.toggled=!hit.toggled;performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);}}
            else if (owner == OWNER_LEFT_STICK)
                leftStick.press(id, x, y);
            else if (owner == OWNER_RIGHT_STICK)
                rightStick.press(id, x, y);
            break;
        }
        case MotionEvent.ACTION_MOVE:
            for (int i = 0; i < event.getPointerCount(); i++) {
                int owner = owners.get(event.getPointerId(i), -1);
                if (owner == OWNER_GAME)
                    forward(event, i, MotionEvent.ACTION_MOVE);
                else if (owner == OWNER_LEFT_STICK)
                    leftStick.move(event.getX(i), event.getY(i));
                else if (owner == OWNER_RIGHT_STICK)
                    rightStick.move(event.getX(i), event.getY(i));
            }
            break;
        case MotionEvent.ACTION_UP:
        case MotionEvent.ACTION_POINTER_UP: {
            int id = event.getPointerId(index);
            int owner = owners.get(id, -1);
            if (owner == OWNER_GAME)
                forward(event, index, MotionEvent.ACTION_UP);
            else if (owner == OWNER_MENU)
                menu.pressed = false;
            else if (owner == OWNER_LEFT_STICK)
                leftStick.release();
            else if (owner == OWNER_RIGHT_STICK)
                rightStick.release();
            owners.delete(id);
            break;
        }
        case MotionEvent.ACTION_CANCEL:
            releaseControls();
            owners.clear();
            return true;
        default:
            break;
        }
        updateHeld(event, action);
        return true;
    }

    // the buttons and D-pad directions under the fingers that hold them (a finger slides from one button
    // to the next), the lifting finger left out
    private void updateHeld(MotionEvent event, int action) {
        int lifted = action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP ? event.getActionIndex() : -1;
        boolean all = action == MotionEvent.ACTION_CANCEL;
        int held = 0, dpadHeld = 0;
        for(Button b:buttons)b.touchHeld=false;
        for (int i = 0; i < event.getPointerCount() && !all; i++) {
            if (i == lifted)
                continue;
            int owner = owners.get(event.getPointerId(i), -1);
            float x = event.getX(i), y = event.getY(i);
            if (owner == OWNER_BUTTONS) {
                Button b = buttonAt(x, y, 4 * unit);
                if (b != null){held |= b.bit;b.touchHeld=true;}
            } else if (owner == OWNER_DPAD)
                dpadHeld |= dpadDirections(x, y);
        }
        int before = 0;
        for (Button b : buttons)
            if (b.pressed && b != menu)
                before |= b.bit;
        before |= dpadBits;
        for (Button b : buttons)
            if (b != menu)
                b.pressed = !b.hidden && (!b.touchButton&&b.toggle?b.toggled:b.touchHeld);
        dpadBits = dpadHeld;
        if (((held | dpadHeld) & ~before) != 0)
            performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY);
        send();
        invalidate();
    }

    // eight ways: a direction counts when the finger is within 67.5 degrees of it
    private int dpadDirections(float x, float y) {
        return DPadControl.directions(x-dpad.centerX(),y-dpad.centerY(),dpad.width());
    }

    // a finger that isn't on a control: to SDL as a touch, as SDLSurface.onTouch sends it
    private void forward(MotionEvent event, int index, int action) {
        float w = Math.max(1, getWidth() - 1), h = Math.max(1, getHeight() - 1);
        float pressure = Math.min(1f, event.getPressure(index));
        SDLActivity.onNativeTouch(event.getDeviceId(), event.getPointerId(index), action,
            event.getX(index) / w, event.getY(index) / h, pressure);
    }

    private void send() {
        syncTouchButtons(false);
        inputs.beginFrame();
        for (Button b : buttons)
            inputs.update(b.sourceSlot, b.pressed ? b.element.commandMask : 0, false, 0, 0, false, 0, 0);
        if (latch != null)
            inputs.update(latch.sourceSlot, latched ? latch.element.commandMask : 0, false, 0, 0, false, 0, 0);
        inputs.update(dpadSource, dpadBits, false, 0, 0, false, 0, 0);
        inputs.update(leftSource, 0, leftStick.pointer >= 0, leftStick.x, leftStick.y, false, 0, 0);
        inputs.update(rightSource, 0, false, 0, 0, rightStick.pointer >= 0, rightStick.x, rightStick.y);
        inputs.finishFrame();
        int held = inputs.outputButtons;
        long now = SystemClock.uptimeMillis();
        for (int i = 0; i < 16; i++)
            if ((held & ~heldButtons & (1 << i)) != 0)
                pressedAt[i] = now;
        heldButtons = held;
        // a press let go too soon stays down until its time is up
        long wait = 0;
        for (int i = 0; i < 16; i++) {
            long left = pressedAt[i] + MIN_PRESS_MS - now;
            if ((sentButtons & ~held & (1 << i)) != 0 && left > 0) {
                held |= 1 << i;
                wait = Math.max(wait, left);
            }
        }
        removeCallbacks(resend);
        if (wait > 0)
            postDelayed(resend, wait);
        held = InputStateAggregator.resolveOpposites(held);
        if (held == sentButtons && inputs.outputLx == sentLx && inputs.outputLy == sentLy && inputs.outputRx == sentRx && inputs.outputRy == sentRy)
            return;
        sentButtons = held;
        sentLx = inputs.outputLx; sentLy = inputs.outputLy;
        sentRx = inputs.outputRx; sentRy = inputs.outputRy;
        setState(held, sentLx, sentLy, sentRx, sentRy);
    }

    // drawing: dark translucent shapes with white outlines, lit while held; the view's alpha is the opacity
    @Override
    protected void onDraw(Canvas canvas) {
        if (!menuButtonShown())
            return;

        float u = unit;
        stroke.setStrokeWidth(2 * u);
        drawButton(canvas, menu, u);
        if (!controlsShown())
            return;
        for (Button b : buttons)
            if (b != menu)
                drawButton(canvas, b, u);
        if (latch != null)
            drawButton(canvas, latch, u);
        drawDpad(canvas, u);
        if(editingLayout){drawActivationArea(canvas,leftStick);drawActivationArea(canvas,rightStick);}
        drawStick(canvas, leftStick, u);
        drawStick(canvas, rightStick, u);
        if(editingLayout){drawEditorTools(canvas);drawTouchTarget(canvas);}
    }

    private void setFill(boolean pressed) {
        fill.setColor(pressed ? 0xa0ffffff : 0x66000000);
        stroke.setColor(pressed ? 0xffffffff : 0xccffffff);
    }

    private void drawButton(Canvas canvas, Button b, float u) {
        if(b.hidden)return;
        setFill(b.pressed);
        float cx = b.rect.centerX(), cy = b.rect.centerY();
        if (b==latch || b.customShape == 1 || (b.customShape == 0 && b.round)) {
            float radius = b.rect.width() / 2;
            canvas.drawCircle(cx, cy, radius, fill);
            canvas.drawCircle(cx, cy, radius, stroke);
        } else {
            float corner = b.customShape == 2 ? 0 : b.customShape == 3 ? b.rect.height() * .15f : b.rect.height() / 2;
            canvas.drawRoundRect(b.rect, corner, corner, fill);
            canvas.drawRoundRect(b.rect, corner, corner, stroke);
        }
        int color = stroke.getColor();
        float s = b.rect.height() * 0.24f; // a symbol's half size
        symbol.setColor(color);
        symbol.setStrokeWidth(Math.max(2 * u, b.rect.height() * 0.07f));
        switch (b.symbol) {
        case 11:
            path.reset();path.moveTo(cx-s*1.25f,cy);
            path.cubicTo(cx-s*.55f,cy-s*1.05f,cx+s*.55f,cy-s*1.05f,cx+s*1.25f,cy);
            path.cubicTo(cx+s*.55f,cy+s*1.05f,cx-s*.55f,cy+s*1.05f,cx-s*1.25f,cy);path.close();
            canvas.drawPath(path,symbol);canvas.drawCircle(cx,cy,s*.46f,symbol);
            break;
        case 10:
            // Touch contact and two symmetric wave arcs, without a hand or screen frame.
            canvas.drawCircle(cx,cy,s*.22f,symbol);
            canvas.drawArc(cx-s*.65f,cy-s*.65f,cx+s*.65f,cy+s*.65f,135,90,false,symbol);
            canvas.drawArc(cx-s*.65f,cy-s*.65f,cx+s*.65f,cy+s*.65f,-45,90,false,symbol);
            canvas.drawArc(cx-s*1.10f,cy-s*1.10f,cx+s*1.10f,cy+s*1.10f,135,90,false,symbol);
            canvas.drawArc(cx-s*1.10f,cy-s*1.10f,cx+s*1.10f,cy+s*1.10f,-45,90,false,symbol);
            break;
        case 6: case 7: case 8: case 9:
            canvas.save();canvas.rotate((b.symbol-6)*90,cx,cy);path.reset();path.moveTo(cx,cy-s);path.lineTo(cx+s,cy+s);path.lineTo(cx-s,cy+s);path.close();canvas.drawPath(path,symbol);canvas.restore();break;
        case SYMBOL_TRIANGLE:
            path.reset();
            path.moveTo(cx, cy - s * 1.05f);
            path.lineTo(cx + s * 1.1f, cy + s * 0.85f);
            path.lineTo(cx - s * 1.1f, cy + s * 0.85f);
            path.close();
            canvas.drawPath(path, symbol);
            break;
        case SYMBOL_CIRCLE:
            canvas.drawCircle(cx, cy, s, symbol);
            break;
        case SYMBOL_CROSS:
            canvas.drawLine(cx - s, cy - s, cx + s, cy + s, symbol);
            canvas.drawLine(cx - s, cy + s, cx + s, cy - s, symbol);
            break;
        case SYMBOL_SQUARE:
            canvas.drawRect(cx - s * 0.9f, cy - s * 0.9f, cx + s * 0.9f, cy + s * 0.9f, symbol);
            break;
        case SYMBOL_MENU:
            for (int i = -1; i <= 1; i++)
                canvas.drawLine(cx - s, cy + i * s * 0.75f, cx + s, cy + i * s * 0.75f, symbol);
            break;
        default:
            text.setColor(stroke.getColor());
            if(b.labelWidth!=b.rect.width()||b.labelHeight!=b.rect.height()){
                text.setTextSize(b.label.length()>1?b.rect.height()*.42f:b.rect.height()*.55f);
                float availableWidth=b.rect.width()*.78f,measured=text.measureText(b.label);
                b.labelSize=measured>availableWidth&&measured>0?text.getTextSize()*availableWidth/measured:text.getTextSize();
                b.labelWidth=b.rect.width();b.labelHeight=b.rect.height();
            }
            text.setTextSize(b.labelSize);
            canvas.drawText(b.label, cx, cy - (text.descent() + text.ascent()) / 2, text);
            break;
        }
    }

    private void drawDpad(Canvas canvas, float u) {
        float previousStrokeWidth=stroke.getStrokeWidth();
        int previousStrokeColor=stroke.getColor();
        float cx=dpad.centerX(),cy=dpad.centerY(),radius=Math.min(dpad.width(),dpad.height())*.5f,inner=radius*.28f;
        int[] bits=DPAD_BITS;
        RectF outer=dpadOuter,hub=dpadHub;outer.set(cx-radius,cy-radius,cx+radius,cy+radius);hub.set(cx-inner,cy-inner,cx+inner,cy+inner);
        setFill(false);canvas.drawCircle(cx,cy,radius,fill);
        for(int i=0;i<4;i++)if((dpadBits&bits[i])!=0){float angle=225+i*90;path.reset();path.arcTo(outer,angle,90);double a=Math.toRadians(angle+90);path.lineTo(cx+inner*(float)Math.cos(a),cy+inner*(float)Math.sin(a));path.arcTo(hub,angle+90,-90);path.close();fill.setColor(0x99ffffff);canvas.drawPath(path,fill);}
        stroke.setColor(0xccffffff);stroke.setStrokeWidth(2 * u);canvas.drawCircle(cx,cy,radius,stroke);canvas.drawCircle(cx,cy,inner,stroke);
        for(int i=0;i<4;i++){double angle=Math.toRadians(45+i*90);float dx=(float)Math.cos(angle),dy=(float)Math.sin(angle);canvas.drawLine(cx+inner*dx,cy+inner*dy,cx+radius*dx,cy+radius*dy,stroke);}
        for(int i=0;i<4;i++){canvas.save();canvas.rotate(90*i,cx,cy);path.reset();path.moveTo(cx,cy-radius*.80f);path.lineTo(cx+radius*.075f,cy-radius*.69f);path.lineTo(cx-radius*.075f,cy-radius*.69f);path.close();if((dpadBits&bits[i])!=0){fill.setColor(0xff202020);canvas.drawPath(path,fill);}else canvas.drawPath(path,stroke);canvas.restore();}
        stroke.setStrokeWidth(previousStrokeWidth);
        stroke.setColor(previousStrokeColor);
    }
    private void drawActivationArea(Canvas canvas,Stick stick){
 if(!stick.available()||stick.mode()!=2)return;
 float reach=AnalogActivation.reach(stick.radius,stick.marginValue,getResources().getDisplayMetrics().density,stick.activationValue);
 editPaint.setStyle(Paint.Style.FILL);editPaint.setColor(0x2238c7ee);canvas.drawCircle(stick.homeX,stick.homeY,reach,editPaint);
 editPaint.setStyle(Paint.Style.STROKE);editPaint.setStrokeWidth(2*unit);editPaint.setColor(0xff38c7ee);editPaint.setPathEffect(new android.graphics.DashPathEffect(new float[]{8*unit,6*unit},0));canvas.drawCircle(stick.homeX,stick.homeY,reach,editPaint);editPaint.setPathEffect(null);
 }
 private void drawStick(Canvas canvas, Stick stick, float u) {
        if(!stick.available())return;
 if(!editingLayout&&stick.mode()==2&&stick.pointer<0&&!stick.idleValue)return;
        boolean held = stick.pointer >= 0;
        float bx = held ? stick.centerX : stick.homeX, by = held ? stick.centerY : stick.homeY;
        setFill(false);
        if(editingLayout||stick.baseValue){canvas.drawCircle(bx, by, stick.radius, fill);canvas.drawCircle(bx, by, stick.radius, stroke);}
        float kx = bx + stick.x * stick.radius, ky = by + stick.y * stick.radius;
        setFill(held);
        if(editingLayout||stick.knobValue){canvas.drawCircle(kx, ky, stick.radius * 0.45f, fill);canvas.drawCircle(kx, ky, stick.radius * 0.45f, stroke);}
    }

    private org.json.JSONObject customLayout = new org.json.JSONObject();
    private boolean editingLayout;
    private int editIndex = -1;
    private int editorPointer = -1, pressedTool = -1;
    private boolean editorDragging;
    private float touchStartX, touchStartY;
    private float dragX, dragY;
    private final android.graphics.Paint editPaint = new android.graphics.Paint();
    private java.util.List<Button> editButtons() {
        java.util.List<Button> all = new java.util.ArrayList<>();
        for(Button b:buttons)if(!b.hidden)all.add(b);
        if(latch != null&&!latch.hidden) all.add(latch);
        return all;
    }
    private String key(int i) {
        java.util.List<Button> all=editButtons();
        if(i<all.size())return all.get(i).element.id;
        int n=directionAt(i);return n==0?dpadElement.id:n==1?leftElement.id:rightElement.id;
    }
    public String[] editorNames() {
        java.util.List<Button> all=editButtons();String[] names=new String[all.size()+1+(leftStick.available()?1:0)+(rightStick.available()?1:0)];
        for(int i=0;i<all.size();i++){Button b=all.get(i);names[i]=b==menu?"Menu":b==latch?"LOCK":b.label!=null?b.label:b.bit==TRIANGLE?"Triangulo":b.bit==CIRCLE?"Circulo":b.bit==CROSS?"X":"Quadrado";}
        int n=all.size();names[n++]="Direcional";if(leftStick.available())names[n++]="Analogico esquerdo";if(rightStick.available())names[n++]="Analogico direito";return names;
    }
    private RectF editorRect(int i) {
        java.util.List<Button> all=editButtons();if(i<all.size())return new RectF(all.get(i).rect);
        int n=directionAt(i);if(n==0)return new RectF(dpad);Stick stick=n==1?leftStick:rightStick;
        return new RectF(stick.homeX-stick.radius,stick.homeY-stick.radius,stick.homeX+stick.radius,stick.homeY+stick.radius);
    }
    private void setEditorRect(int i,RectF rect,int shape) {
        java.util.List<Button> all=editButtons();if(i<all.size()){all.get(i).rect.set(rect);all.get(i).customShape=shape;return;}
        int n=directionAt(i);if(n==0){dpad.set(rect);return;}Stick stick=n==1?leftStick:rightStick;
        stick.homeX=rect.centerX();stick.homeY=rect.centerY();stick.radius=Math.min(rect.width(),rect.height())/2;
    }
    public void reloadCustomLayout(){
        if(editingLayout)return;
        try{customLayout=new org.json.JSONObject(getContext().getSharedPreferences("touch-layout",0).getString("layout","{}"));}
        catch(Exception e){customLayout=new org.json.JSONObject();}
 loadAnalogSettings();
 if(menu!=null)reloadExtraButtons();
    }
    private void applyCustomLayout(){
        if(getWidth()==0||getHeight()==0)return;
        org.json.JSONObject hidden=customLayout.optJSONObject("hidden_buttons");
        for(Button b:buttons)b.hidden=b!=menu&&hidden!=null&&hidden.optBoolean(b.element.id,false);
        if(latch!=null)latch.hidden=hidden!=null&&hidden.optBoolean("lock",false);
        for(int i=0;i<editorNames().length;i++){
            org.json.JSONObject v=customLayout.optJSONObject(key(i));if(v==null)continue;
            float scale=Math.min(getWidth(),getHeight());
            float w=(float)v.optDouble("w",.08)*scale,h=(float)v.optDouble("h",.08)*scale;
            w=Math.max(24,Math.min(w,getWidth()));h=Math.max(24,Math.min(h,getHeight()));
            float x=clamp((float)v.optDouble("x",.5)*getWidth(),w/2,getWidth()-w/2);
            float y=clamp((float)v.optDouble("y",.5)*getHeight(),h/2,getHeight()-h/2);
            setEditorRect(i,new RectF(x-w/2,y-h/2,x+w/2,y+h/2),v.optInt("shape",0));
        }
    }
    private void remember(int i,RectF r,int shape){
        try{org.json.JSONObject v=new org.json.JSONObject();v.put("x",r.centerX()/getWidth());v.put("y",r.centerY()/getHeight());
            v.put("w",r.width()/Math.min(getWidth(),getHeight()));v.put("h",r.height()/Math.min(getWidth(),getHeight()));v.put("shape",shape);customLayout.put(key(i),v);
        }catch(Exception ignored){}applyCustomLayout();invalidate();
    }
    public void beginLayoutEditor(){editingLayout=true;editIndex=-1;invalidate();}
    public void selectEditorControl(int index){editIndex=Math.max(0,Math.min(index,editorNames().length-1));invalidate();}
    public void resizeEditorControl(float factor){RectF r=editorRect(editIndex);float w=r.width()*factor,h=r.height()*factor;java.util.List<Button> all=editButtons();int shape=editIndex<all.size()?all.get(editIndex).customShape:0;remember(editIndex,new RectF(r.centerX()-w/2,r.centerY()-h/2,r.centerX()+w/2,r.centerY()+h/2),shape);}
    public void shapeEditorControl(int shape){RectF r=editorRect(editIndex);if(editIndex<editButtons().size()&&(shape==1||shape==2)){float side=Math.min(r.width(),r.height());r.set(r.centerX()-side/2,r.centerY()-side/2,r.centerX()+side/2,r.centerY()+side/2);}remember(editIndex,r,shape);}
    public void saveEditorLayout(){getContext().getSharedPreferences("touch-layout",0).edit().putString("layout",customLayout.toString()).apply();}
    public void resetEditorLayout(){customLayout=new org.json.JSONObject();loadAnalogSettings();reloadExtraButtons();for(Button b:buttons)b.customShape=0;if(latch!=null)latch.customShape=0;editIndex=-1;layoutControls();invalidate();}

    private float[] editorTools(){RectF r=editorRect(editIndex);return EditorGeometry.tools(r.left,r.top,r.right,r.bottom,getWidth(),getHeight(),getResources().getDisplayMetrics().density);}
    private int toolAt(float x,float y){if(editIndex<0)return -1;float[] tools=editorTools();for(int i=0;i<3;i++){if(i==0&&editIndex==editButtons().size())continue;float dx=x-tools[i*2],dy=y-tools[i*2+1];if(dx*dx+dy*dy<=tools[6]*tools[6])return i;}return -1;}
    private int controlAt(float x,float y){int best=-1;float score=Float.MAX_VALUE;java.util.List<Button> all=editButtons();for(int i=0;i<editorNames().length;i++){RectF r=editorRect(i);if(!r.contains(x,y))continue;boolean round=i>=all.size()+1||(i<all.size()&&(all.get(i)==latch||all.get(i).customShape==1||(all.get(i).customShape==0&&all.get(i).round)));if(round){float dx=(x-r.centerX())/(r.width()/2),dy=(y-r.centerY())/(r.height()/2);if(dx*dx+dy*dy>1)continue;}float area=r.width()*r.height();if(area<score){score=area;best=i;}}return best;}
    private void cycleEditorShape(){
        java.util.List<Button> all=editButtons();if(editIndex>=all.size()){android.widget.Toast.makeText(getContext(),"Formato fixo; use + e - para ajustar o tamanho",android.widget.Toast.LENGTH_SHORT).show();return;}
        Button b=all.get(editIndex);if(b==latch){toast("LOCK usa formato circular; ajuste o tamanho com + e -");return;}int current=b.customShape==0?(b.round?1:3):b.customShape,next=current==1?2:current==2?3:1;
        RectF r=editorRect(editIndex);float side=Math.min(r.width(),r.height()),cx=r.centerX(),cy=r.centerY();
        if(next==3)r.set(cx-side,cy-side/2,cx+side,cy+side/2);else r.set(cx-side/2,cy-side/2,cx+side/2,cy+side/2);
        remember(editIndex,r,next);
    }
    private void drawEditorTools(Canvas canvas){
        if(editIndex<0)return;float density=getResources().getDisplayMetrics().density;RectF r=editorRect(editIndex);
        editPaint.setStyle(Paint.Style.STROKE);editPaint.setStrokeWidth(3*density);editPaint.setColor(0xffffdc79);canvas.drawRoundRect(r,8*density,8*density,editPaint);
        float[] tools=editorTools();float radius=tools[6];for(int i=0;i<3;i++){if(i==0&&editIndex==editButtons().size())continue;float x=tools[i*2],y=tools[i*2+1];editPaint.setStyle(Paint.Style.FILL);editPaint.setColor(pressedTool==i?0xff967b31:0xed29323f);canvas.drawCircle(x,y,radius,editPaint);editPaint.setStyle(Paint.Style.STROKE);editPaint.setStrokeWidth(2*density);editPaint.setColor(0xfff5f5f5);canvas.drawCircle(x,y,radius-1,editPaint);
          float arm=radius*.4f;if(i==0&&editIndex>editButtons().size()){editPaint.setStyle(Paint.Style.FILL);editPaint.setTextAlign(Paint.Align.CENTER);editPaint.setTextSize(radius*1.3f);canvas.drawText("\u2699",x,y+radius*.45f,editPaint);}else if(i==0){canvas.drawRect(x-arm,y-arm,x+arm*.45f,y+arm*.45f,editPaint);canvas.drawCircle(x+arm*.35f,y+arm*.35f,arm*.8f,editPaint);}else {canvas.drawLine(x-arm,y,x+arm,y,editPaint);if(i==1)canvas.drawLine(x,y-arm,x,y+arm,editPaint);}}
    }
    private boolean editTouch(MotionEvent e){
        int action=e.getActionMasked();
        if(action==MotionEvent.ACTION_DOWN){editorPointer=e.getPointerId(0);dragX=touchStartX=e.getX();dragY=touchStartY=e.getY();editorDragging=false;pressedTool=toolAt(dragX,dragY);if(pressedTool<0)editIndex=controlAt(dragX,dragY);invalidate();return true;}
        if(action==MotionEvent.ACTION_CANCEL||(action==MotionEvent.ACTION_POINTER_UP&&e.getPointerId(e.getActionIndex())==editorPointer)){pressedTool=-1;editorPointer=-1;editorDragging=false;invalidate();return true;}
        int index=e.findPointerIndex(editorPointer);if(index<0)return true;float x=e.getX(index),y=e.getY(index);
        if(action==MotionEvent.ACTION_MOVE&&pressedTool<0&&editIndex>=0){float dx=x-touchStartX,dy=y-touchStartY,slop=android.view.ViewConfiguration.get(getContext()).getScaledTouchSlop();if(editorDragging||dx*dx+dy*dy>slop*slop){editorDragging=true;RectF r=editorRect(editIndex);r.offset(x-dragX,y-dragY);java.util.List<Button> all=editButtons();remember(editIndex,r,editIndex<all.size()?all.get(editIndex).customShape:0);dragX=x;dragY=y;}return true;}
        if(action==MotionEvent.ACTION_UP){int tool=pressedTool;if(tool>=0&&toolAt(x,y)==tool){if(tool==0){if(editIndex>editButtons().size())configureAnalog(directionAt(editIndex)==1);else cycleEditorShape();}else resizeEditorControl(tool==1?1.1f:.9f);performHapticFeedback(android.view.HapticFeedbackConstants.CLOCK_TICK);}pressedTool=-1;editorPointer=-1;editorDragging=false;invalidate();return true;}
        return true;
    }
    @Override public void onWindowFocusChanged(boolean focus){super.onWindowFocusChanged(focus);if(!focus)releaseControls();if(focus&&!editingLayout){reloadCustomLayout();layoutControls();invalidate();}}


    private void loadAnalogSettings(){org.json.JSONObject a=customLayout.optJSONObject("analog_settings");leftStick.config=a==null?null:a.optJSONObject("left");rightStick.config=a==null?null:a.optJSONObject("right");if(leftStick.config==null)leftStick.config=new org.json.JSONObject();if(rightStick.config==null)rightStick.config=new org.json.JSONObject();migrateAnalogVisibility(leftStick.config);migrateAnalogVisibility(rightStick.config);leftStick.refresh();rightStick.refresh();}
 private void migrateAnalogVisibility(org.json.JSONObject config){if(config.optInt("visibilityVersion",0)<1)try{config.put("idle",false);config.put("visibilityVersion",1);}catch(org.json.JSONException e){throw new IllegalStateException(e);}}
 private void configureAnalog(boolean left){AnalogOptions.show(getContext(),left,(left?leftStick:rightStick).config,updated->{try{org.json.JSONObject all=customLayout.optJSONObject("analog_settings");if(all==null){all=new org.json.JSONObject();customLayout.put("analog_settings",all);}all.put(left?"left":"right",updated);loadAnalogSettings();invalidate();}catch(org.json.JSONException e){throw new IllegalStateException(e);}});}


    private int directionAt(int index){int n=index-editButtons().size();if(n==0)return 0;if(n==1&&leftStick.available())return 1;return 2;}
    private void setAnalogEnabled(boolean left,boolean enabled){
        try{org.json.JSONObject all=customLayout.optJSONObject("analog_settings");if(all==null){all=new org.json.JSONObject();customLayout.put("analog_settings",all);}org.json.JSONObject c=(left?leftStick:rightStick).config;c.put("enabled",enabled);all.put(left?"left":"right",c);loadAnalogSettings();}catch(org.json.JSONException e){throw new IllegalStateException(e);}invalidate();
    }
    private void restoreAnalog(boolean left){Stick stick=left?leftStick:rightStick;if(stick.available())toast("Esse analogico ja esta no layout");else{try{stick.config.put("idle",false);stick.config.put("visibilityVersion",1);}catch(org.json.JSONException e){throw new IllegalStateException(e);}setAnalogEnabled(left,true);layoutControls();}editIndex=editButtons().size()+1+(left?0:leftStick.available()?1:0);invalidate();}
    private void editBinding(){Button b=selectedButton();if(b==null||!b.extra||b.touchButton){toast("Selecione um botao adicional comum ou combinado");return;}bindingDialog(b);}
    private void bindingDialog(Button selected){
        String[] names={"SELECT","START","Cima","Direita","Baixo","Esquerda","L","R","Triangulo","Circulo","X","Quadrado"};int[] masks={SELECT,START,UP,RIGHT,DOWN,LEFT,L1,R1,TRIANGLE,CIRCLE,CROSS,SQUARE};
        android.widget.LinearLayout content=new android.widget.LinearLayout(getContext());content.setOrientation(1);
        android.widget.TextView help=new android.widget.TextView(getContext());help.setText("Marque um ou mais comandos. Normal: mantem enquanto voce segura. Travar: um toque segura, outro solta. Abrir o menu ou sair do app solta todos.");content.addView(help);
        android.widget.TextView preview=new android.widget.TextView(getContext());preview.setText(selected==null?"O nome mostra automaticamente os comandos escolhidos":ButtonAppearance.label(selected.bit));content.addView(preview);
        android.widget.CheckBox toggle=new android.widget.CheckBox(getContext());toggle.setText("Travar pressionado (liga/desliga)");toggle.setChecked(selected!=null&&selected.toggle);content.addView(toggle);
        android.widget.CheckBox[] checks=new android.widget.CheckBox[names.length];for(int i=0;i<names.length;i++){checks[i]=new android.widget.CheckBox(getContext());checks[i].setText(names[i]);checks[i].setChecked(selected!=null&&(selected.bit&masks[i])!=0);content.addView(checks[i]);}
        android.widget.ScrollView scroll=new android.widget.ScrollView(getContext());scroll.addView(content);
        android.app.AlertDialog dialog=new android.app.AlertDialog.Builder(getContext()).setTitle(selected==null?"Combinacao de botoes":"Alterar comando").setView(scroll).setPositiveButton("Aplicar",null).setNegativeButton("Cancelar",null).create();
        dialog.setOnShowListener(d->dialog.getButton(-1).setOnClickListener(v->{int mask=0;for(int i=0;i<masks.length;i++)if(checks[i].isChecked())mask|=masks[i];if(mask==0){toast("Escolha pelo menos um comando");return;}if((mask&0x50)==0x50||(mask&0xa0)==0xa0){toast("Nao combine direcoes opostas");return;}
            String name=ButtonAppearance.label(mask);
            if(selected==null){if(extraRecords().length()>=24){toast("Limite de 24 botoes adicionais");return;}addExtra(mask,name,false,.5f,.5f,null,1);}
            Button target=selected==null?selectedButton():selected;
            org.json.JSONArray a=extraRecords();for(int i=0;i<a.length();i++){org.json.JSONObject item=a.optJSONObject(i);if(item!=null&&target.element.id.equals("custom_"+item.optInt("slot",-1)))try{item.put("bit",mask);item.put("label",name);item.put("toggle",toggle.isChecked());}catch(org.json.JSONException e){throw new IllegalStateException(e);}}
            reloadExtraButtons();layoutControls();invalidate();dialog.dismiss();
        }));dialog.show();
    }

    private boolean pickingTarget;
    private String targetButtonId;
    private void toast(String message){android.widget.Toast.makeText(getContext(),message,android.widget.Toast.LENGTH_SHORT).show();}
    private void reloadExtraButtons(){
        String selectedId=editIndex>=0&&editIndex<editorNames().length?key(editIndex):null;
        syncTouchButtons(true);
        for(int i=buttons.size()-1;i>=0;i--)if(buttons.get(i).extra)buttons.remove(i);
        org.json.JSONArray list=customLayout.optJSONArray("extra_buttons");
        boolean[] used=new boolean[24];
        if(list!=null)for(int i=0;i<list.length();i++){
            org.json.JSONObject v=list.optJSONObject(i);if(v==null)continue;
            int slot=v.optInt("slot",-1);if(slot<0||slot>=24||used[slot])continue;used[slot]=true;
            int mask=v.optInt("bit",0);boolean touch=v.optBoolean("touch",false);
            if(!touch&&!validDigital(mask))continue;
            String label=touch?"Touch":ButtonAppearance.label(mask);
            Button x=new Button(touch?0:mask,touch?10:ButtonAppearance.symbol(mask),label,Color.WHITE,true);
            x.extra=true;x.touchButton=touch;x.toggle=!touch&&v.optBoolean("toggle",false);x.syntheticId=1000+slot;
            x.targetX=TouchTargetMap.safe((float)v.optDouble("targetX",.5));x.targetY=TouchTargetMap.safe((float)v.optDouble("targetY",.5));
            x.element=new ControlElement("custom_"+slot,ControlElement.Type.BUTTON,800,touch?0:mask);
            x.sourceSlot=inputs.register(x.element);buttons.add(x);
        }
        // Rebuilt controls have empty rectangles. Restore saved geometry immediately,
        // including after deletion (which previously only invalidated the View).
        applyCustomLayout();
        editIndex=-1;
        if(selectedId!=null)for(int i=0;i<editorNames().length;i++)if(selectedId.equals(key(i))){editIndex=i;break;}
    }
    private static boolean validDigital(int bit){int allowed=SELECT|START|UP|RIGHT|DOWN|LEFT|L1|R1|TRIANGLE|CIRCLE|CROSS|SQUARE;return bit!=0&&(bit&~allowed)==0;}
    private void syncTouchButtons(boolean cancel){
        if(buttons==null)return;
        for(Button b:buttons)if(b.touchButton){
            boolean down=!cancel&&!editingLayout&&!settingsMenuOpen&&controlsShown()&&!b.hidden&&b.pressed;
            if(down!=b.touchSent){
                float nx=TouchTargetMap.screenX(b.targetX,getWidth(),getHeight()),ny=TouchTargetMap.screenY(b.targetY,getWidth(),getHeight());
                SDLActivity.onNativeTouch(0,b.syntheticId,down?MotionEvent.ACTION_DOWN:cancel?MotionEvent.ACTION_CANCEL:MotionEvent.ACTION_UP,nx,ny,down?1:0);
                b.touchSent=down;
            }
        }
    }
    public void editorButtonMenu(){
        new android.app.AlertDialog.Builder(getContext()).setTitle("Botoes").setItems(new String[]{"Adicionar controle","Duplicar selecionado","Remover selecionado","Escolher alvo touch","Alterar comando / segurar"},(d,w)->{
            if(w==0)addButtonDialog();else if(w==1)duplicateSelected();else if(w==2)removeSelected();else if(w==3)pickTouchTarget();else editBinding();
        }).show();
    }
    private Button selectedButton(){java.util.List<Button> all=editButtons();return editIndex>=0&&editIndex<all.size()?all.get(editIndex):null;}
    private void addButtonDialog(){
        String[] labels={"Triangulo","Circulo","X","Quadrado","L","R","SELECT","START","Cima","Direita","Baixo","Esquerda","Toque na tela","Combinacao de botoes","Analogico esquerdo","Analogico direito"};
        int[] bits={TRIANGLE,CIRCLE,CROSS,SQUARE,L1,R1,SELECT,START,UP,RIGHT,DOWN,LEFT,0};
        new android.app.AlertDialog.Builder(getContext()).setTitle("Adicionar botao").setItems(labels,(d,w)->{if(w>=14)restoreAnalog(w==14);else if(w==13)bindingDialog(null);else addExtra(bits[w],labels[w],w==12,.5f,.5f,null,1);}).show();
    }
    private org.json.JSONArray extraRecords(){org.json.JSONArray a=customLayout.optJSONArray("extra_buttons");if(a==null){a=new org.json.JSONArray();try{customLayout.put("extra_buttons",a);}catch(org.json.JSONException e){throw new IllegalStateException(e);}}return a;}
    private void addExtra(int bit,String label,boolean touch,float tx,float ty,RectF source,int shape){
        org.json.JSONArray a=extraRecords();boolean[] used=new boolean[24];for(int i=0;i<a.length();i++){org.json.JSONObject v=a.optJSONObject(i);if(v!=null){int slot=v.optInt("slot",-1);if(slot>=0&&slot<24)used[slot]=true;}}
        int slot=0;while(slot<24&&used[slot])slot++;if(slot==24){toast("Limite de 24 botoes adicionais");return;}
        try{org.json.JSONObject v=new org.json.JSONObject();v.put("slot",slot);v.put("bit",bit);v.put("label",label);v.put("touch",touch);v.put("targetX",tx);v.put("targetY",ty);a.put(v);reloadExtraButtons();
            String id="custom_"+slot;java.util.List<Button> all=editButtons();for(int i=0;i<all.size();i++)if(all.get(i).element.id.equals(id)){editIndex=i;break;}
            float side=Math.min(getWidth(),getHeight())*.14f;RectF r=source==null?new RectF(getWidth()*.5f-side/2,getHeight()*.5f-side/2,getWidth()*.5f+side/2,getHeight()*.5f+side/2):new RectF(source);
            if(source!=null)r.offset(24*getResources().getDisplayMetrics().density,24*getResources().getDisplayMetrics().density);
            remember(editIndex,r,shape);if(touch)toast("Use Botoes > Escolher alvo touch");
        }catch(org.json.JSONException e){throw new IllegalStateException(e);}
    }
    private void duplicateSelected(){Button b=selectedButton();if(b==null||b==menu||b==latch){toast("Selecione um botao comum ou touch para duplicar");return;}if(extraRecords().length()>=24){toast("Limite de 24 botoes adicionais");return;}addExtra(b.bit,b.label!=null?b.label:editorNames()[editIndex],b.touchButton,b.targetX,b.targetY,editorRect(editIndex),b.customShape==0?(b.round?1:3):b.customShape);Button copy=selectedButton();if(copy!=null&&b.toggle){org.json.JSONArray records=extraRecords();for(int i=0;i<records.length();i++){org.json.JSONObject item=records.optJSONObject(i);if(item!=null&&copy.element.id.equals("custom_"+item.optInt("slot",-1)))try{item.put("toggle",true);}catch(org.json.JSONException e){throw new IllegalStateException(e);}}reloadExtraButtons();applyCustomLayout();}}
    private void removeSelected(){Button b=selectedButton();if(b==null){if(editIndex>editButtons().size()){setAnalogEnabled(directionAt(editIndex)==1,false);editIndex=-1;invalidate();}else toast("Selecione um botao ou analogico para remover");return;}if(b==menu){toast("O menu permanece disponivel para acessar os ajustes");return;}
        new android.app.AlertDialog.Builder(getContext()).setMessage("Remover "+(b.label!=null?b.label:ButtonAppearance.label(b.bit))+"? Salvar confirma; Cancelar descarta as alteracoes.").setPositiveButton("Remover",(d,w)->{
            try{if(b.extra){org.json.JSONArray a=extraRecords(),out=new org.json.JSONArray();for(int i=0;i<a.length();i++){org.json.JSONObject v=a.optJSONObject(i);if(v!=null&&!b.element.id.equals("custom_"+v.optInt("slot",-1)))out.put(v);}customLayout.put("extra_buttons",out);customLayout.remove(b.element.id);reloadExtraButtons();}
            else{org.json.JSONObject h=customLayout.optJSONObject("hidden_buttons");if(h==null){h=new org.json.JSONObject();customLayout.put("hidden_buttons",h);}h.put(b.element.id,true);b.hidden=true;}
            editIndex=-1;invalidate();}catch(org.json.JSONException e){throw new IllegalStateException(e);}
        }).setNegativeButton("Cancelar",null).show();
    }
    private void pickTouchTarget(){Button b=selectedButton();if(b==null||!b.touchButton){toast("Selecione um botao touch primeiro");return;}targetButtonId=b.element.id;pickingTarget=true;toast("Toque na imagem do Vita para escolher o alvo");invalidate();}
    private boolean targetTouch(MotionEvent e){if(e.getActionMasked()==MotionEvent.ACTION_CANCEL){pickingTarget=false;invalidate();return true;}if(e.getActionMasked()==MotionEvent.ACTION_UP){float x=TouchTargetMap.vitaX(e.getX(),getWidth(),getHeight()),y=TouchTargetMap.vitaY(e.getY(),getWidth(),getHeight());if(x<0||x>1||y<0||y>1){toast("Escolha um ponto dentro da imagem do Vita");return true;}
        org.json.JSONArray a=extraRecords();for(int i=0;i<a.length();i++){org.json.JSONObject v=a.optJSONObject(i);if(v!=null&&targetButtonId.equals("custom_"+v.optInt("slot",-1)))try{v.put("targetX",x);v.put("targetY",y);}catch(org.json.JSONException ex){throw new IllegalStateException(ex);}}
        for(Button b:buttons)if(b.element.id.equals(targetButtonId)){b.targetX=x;b.targetY=y;}pickingTarget=false;invalidate();toast("Alvo definido. Toque Salvar para manter.");}return true;}
    private void drawTouchTarget(Canvas c){Button b=selectedButton();if(b==null||!b.touchButton)return;float x=TouchTargetMap.screenX(b.targetX,getWidth(),getHeight())*getWidth(),y=TouchTargetMap.screenY(b.targetY,getWidth(),getHeight())*getHeight();editPaint.setStyle(Paint.Style.STROKE);editPaint.setStrokeWidth(2*unit);editPaint.setColor(0xffff6b6b);c.drawCircle(x,y,12*unit,editPaint);c.drawLine(x-20*unit,y,x+20*unit,y,editPaint);c.drawLine(x,y-20*unit,x,y+20*unit,editPaint);}

    private void rememberPointers(MotionEvent event) {
        float w=Math.max(1,getWidth()-1),h=Math.max(1,getHeight()-1);
        for(int i=0;i<event.getPointerCount();i++)owners.remember(event.getPointerId(i),event.getDeviceId(),event.getX(i)/w,event.getY(i)/h);
    }
    private void clearPendingVirtualInput() {
        removeCallbacks(resend);removeCallbacks(analogFrame);analogScheduled=false;
        java.util.Arrays.fill(pressedAt,0);
        heldButtons=0; inputs.clear();
        if(sentButtons!=0||sentLx!=0||sentLy!=0||sentRx!=0||sentRy!=0)setState(0,0,0,0,0);
        sentButtons=0; sentLx=sentLy=sentRx=sentRy=0;
    }

}
