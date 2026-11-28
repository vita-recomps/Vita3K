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

/** Activation uses the saved anchor, never the moving floating base. */
public final class AnalogActivation {
    public static float reach(float radius, float marginDp, float density, float scale) {
        return (radius + Math.max(0, Math.min(80, marginDp)) * density)
                * Math.max(1, Math.min(3, Float.isFinite(scale) ? scale : 1.5f));
    }

    public static boolean contains(
            float x,
            float y,
            float homeX,
            float homeY,
            float radius,
            float marginDp,
            float density,
            float scale) {
        if (!Float.isFinite(x)
                || !Float.isFinite(y)
                || !Float.isFinite(radius)
                || !Float.isFinite(marginDp)
                || !Float.isFinite(density)
                || radius < 0
                || density <= 0) return false;
        double reach = reach(radius, marginDp, density, scale);
        return Math.hypot(x - homeX, y - homeY) <= reach;
    }
}
