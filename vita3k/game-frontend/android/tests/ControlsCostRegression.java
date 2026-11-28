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

import java.util.*;

public class ControlsCostRegression {
    static class SystemClock {
        static long time;

        static long uptimeMillis() {
            return time;
        }
    }

    static class Settings extends HashMap<String, Object> {
        int optInt(String k, int d) {
            Object v = get(k);
            return v instanceof Number ? ((Number) v).intValue() : d;
        }

        double optDouble(String k, double d) {
            Object v = get(k);
            return v instanceof Number ? ((Number) v).doubleValue() : d;
        }

        boolean optBoolean(String k, boolean d) {
            Object v = get(k);
            return v instanceof Boolean ? (Boolean) v : d;
        }
    }

    float density = 2;
    int schedules;

    int getWidth() {
        return 2000;
    }

    int getHeight() {
        return 1000;
    }

    void scheduleAnalog() {
        schedules++;
    }

    static void same(float a, float b) {
        if (Math.abs(a - b) > .00001f || Float.isNaN(a) != Float.isNaN(b))
            throw new AssertionError(a + " != " + b);
    }

    final class OldStick {
        Settings config = new Settings();
        final FloatingAnalog floating = new FloatingAnalog();

        boolean available() {
            return config.optBoolean("enabled", true);
        }

        int mode() {
            return config.optInt("mode", 0);
        }

        float option(String k, float fallback) {
            return (float) config.optDouble(k, fallback);
        }

        boolean region(float x, float y) {
            return AnalogActivation.contains(
                    x,
                    y,
                    homeX,
                    homeY,
                    radius,
                    option("margin", 24),
                    density,
                    option("activationScale", 1.5f));
        }

        boolean tick() {
            if (pointer < 0 || mode() == 0) return false;
            boolean pending = floating.tick(SystemClock.uptimeMillis(), option("smoothing", 0));
            x = floating.x;
            y = floating.y;
            centerX = floating.baseX;
            centerY = floating.baseY;
            return pending;
        }

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
            if (mode() != 0) {
                floating.press(
                        mode() == 1 ? homeX : px,
                        mode() == 1 ? homeY : py,
                        SystemClock.uptimeMillis());
                move(px, py);
            }
        }

        void move(float px, float py) {
            if (mode() != 0) {
                floating.move(
                        px,
                        py,
                        Math.max(20, Math.min(160, option("radius", 60))) * density,
                        mode() == 2 && config.optBoolean("follow", false),
                        option("sx", 1),
                        option("sy", 1),
                        option("dead", .08f),
                        option("maximum", 1),
                        config.optInt("curve", 0),
                        config.optInt("restrict", 0),
                        config.optBoolean("invertX", false),
                        config.optBoolean("invertY", false));
                tick();
                scheduleAnalog();
                return;
            }
            analog.move(px - centerX, py - centerY, radius);
            x = analog.x;
            y = analog.y;
        }

        void release() {
            analog.release();
            floating.release();
            pointer = -1;
            x = y = 0;
        }
    }

    final class NewStick {
        Settings config = new Settings();
        final FloatingAnalog floating = new FloatingAnalog();
        int modeValue, curveValue, restrictionValue;
        boolean availableValue = true,
                followValue,
                invertXValue,
                invertYValue,
                idleValue,
                baseValue = true,
                knobValue = true;
        float marginValue = 24,
                activationValue = 1.5f,
                radiusValue = 60,
                sxValue = 1,
                syValue = 1,
                deadValue = .08f,
                maximumValue = 1,
                smoothingValue;

        boolean available() {
            return availableValue;
        }

        int mode() {
            return modeValue;
        }

        float option(String k, float fallback) {
            return (float) config.optDouble(k, fallback);
        }

        void refresh() {
            modeValue = config.optInt("mode", 0);
            availableValue = config.optBoolean("enabled", true);
            marginValue = option("margin", 24);
            activationValue = option("activationScale", 1.5f);
            radiusValue = Math.max(20, Math.min(160, option("radius", 60)));
            sxValue = option("sx", 1);
            syValue = option("sy", 1);
            deadValue = option("dead", .08f);
            maximumValue = option("maximum", 1);
            smoothingValue = option("smoothing", 0);
            curveValue = config.optInt("curve", 0);
            restrictionValue = config.optInt("restrict", 0);
            followValue = config.optBoolean("follow", false);
            invertXValue = config.optBoolean("invertX", false);
            invertYValue = config.optBoolean("invertY", false);
            idleValue = config.optBoolean("idle", false);
            baseValue = config.optBoolean("base", true);
            knobValue = config.optBoolean("knob", true);
        }

        boolean region(float x, float y) {
            return AnalogActivation.contains(
                    x, y, homeX, homeY, radius, marginValue, density, activationValue);
        }

        boolean tick() {
            if (pointer < 0 || mode() == 0) return false;
            boolean pending = floating.tick(SystemClock.uptimeMillis(), smoothingValue);
            x = floating.x;
            y = floating.y;
            centerX = floating.baseX;
            centerY = floating.baseY;
            return pending;
        }

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
            if (mode() != 0) {
                floating.press(
                        mode() == 1 ? homeX : px,
                        mode() == 1 ? homeY : py,
                        SystemClock.uptimeMillis());
                move(px, py);
            }
        }

        void move(float px, float py) {
            if (mode() != 0) {
                floating.move(
                        px,
                        py,
                        radiusValue * density,
                        mode() == 2 && followValue,
                        sxValue,
                        syValue,
                        deadValue,
                        maximumValue,
                        curveValue,
                        restrictionValue,
                        invertXValue,
                        invertYValue);
                if (tick()) scheduleAnalog();
                return;
            }
            analog.move(px - centerX, py - centerY, radius);
            x = analog.x;
            y = analog.y;
        }

        void release() {
            analog.release();
            floating.release();
            pointer = -1;
            x = y = 0;
        }
    }

    public static void main(String[] args) {
        ControlsCostRegression h = new ControlsCostRegression();
        int samples = 0, oldUnsmoothed = 0, newUnsmoothed = 0;
        for (int mode = 0; mode <= 2; mode++)
            for (int smooth = 0; smooth < 3; smooth++)
                for (int restrict = 0; restrict < 5; restrict++) {
                    OldStick old = h.new OldStick();
                    NewStick opt = h.new NewStick();
                    Settings settings = new Settings();
                    settings.put("mode", mode);
                    settings.put("smoothing", smooth * .3);
                    settings.put("restrict", restrict);
                    settings.put("follow", true);
                    settings.put("sx", 1.4);
                    settings.put("sy", .8);
                    settings.put("invertY", true);
                    settings.put("curve", restrict % 3);
                    old.config = settings;
                    opt.config = settings;
                    opt.refresh();
                    old.radius = opt.radius = 60;
                    old.homeX = opt.homeX = 120;
                    old.homeY = opt.homeY = 150;
                    SystemClock.time = 0;
                    old.press(7, 100, 100);
                    opt.press(7, 100, 100);
                    for (int i = 1; i <= 500; i++) {
                        SystemClock.time = i * 16;
                        float x = 100 + (float) Math.sin(i * .07) * 180,
                                y = 100 + (float) Math.cos(i * .09) * 160;
                        h.schedules = 0;
                        old.move(x, y);
                        int before = h.schedules;
                        h.schedules = 0;
                        opt.move(x, y);
                        int after = h.schedules;
                        if (smooth == 0) {
                            oldUnsmoothed += before;
                            newUnsmoothed += after;
                        }
                        same(old.x, opt.x);
                        same(old.y, opt.y);
                        same(old.centerX, opt.centerX);
                        same(old.centerY, opt.centerY);
                        samples++;
                    }
                    old.release();
                    opt.release();
                    same(old.x, opt.x);
                    same(old.y, opt.y);
                    if (opt.pointer != -1) throw new AssertionError("release");
                }
        if (newUnsmoothed != 0 || oldUnsmoothed == 0)
            throw new AssertionError("redundant scheduling");
        System.out.println(
                "PASS "
                        + samples
                        + " samples using actual old/new Stick classes: identical"
                        + " axes/base/release; unsmoothed schedule requests "
                        + oldUnsmoothed
                        + " -> "
                        + newUnsmoothed
                        + " in test sequence");
    }
}
