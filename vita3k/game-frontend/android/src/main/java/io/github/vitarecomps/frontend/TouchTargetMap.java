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

/** Same centered 960x544 fit used by native touch spots in OnScreenControls. */
public final class TouchTargetMap {
    public static float safe(float v) {
        return Float.isFinite(v) ? Math.max(0, Math.min(1, v)) : .5f;
    }

    private static float scale(float w, float h) {
        return Math.min(Math.max(1, w) / 960f, Math.max(1, h) / 544f);
    }

    public static float screenX(float x, float w, float h) {
        float s = scale(w, h);
        return ((w - 960 * s) / 2 + safe(x) * 960 * s) / Math.max(1, w);
    }

    public static float screenY(float y, float w, float h) {
        float s = scale(w, h);
        return ((h - 544 * s) / 2 + safe(y) * 544 * s) / Math.max(1, h);
    }

    public static float vitaX(float x, float w, float h) {
        float s = scale(w, h);
        return (x - (w - 960 * s) / 2) / (960 * s);
    }

    public static float vitaY(float y, float w, float h) {
        float s = scale(w, h);
        return (y - (h - 544 * s) / 2) / (544 * s);
    }
}
