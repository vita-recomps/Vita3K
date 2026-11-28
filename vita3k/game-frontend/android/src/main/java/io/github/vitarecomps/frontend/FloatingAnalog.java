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

/** Pure touch response model. Release is always immediate; time is monotonic milliseconds. */
public final class FloatingAnalog {
    public float x, y, baseX, baseY;
    private float tx, ty;
    private long last;

    public void press(float bx, float by, long now) {
        baseX = bx;
        baseY = by;
        x = y = tx = ty = 0;
        last = now;
    }

    public void move(
            float px,
            float py,
            float radius,
            boolean follow,
            float sx,
            float sy,
            float dead,
            float maximum,
            int curve,
            int restrict,
            boolean ix,
            boolean iy) {
        if (!(radius > 0) || !Float.isFinite(px) || !Float.isFinite(py)) {
            release();
            return;
        }
        float dx = px - baseX, dy = py - baseY, len = (float) Math.hypot(dx, dy);
        if (follow && len > radius) {
            baseX += dx * (1 - radius / len);
            baseY += dy * (1 - radius / len);
            dx = px - baseX;
            dy = py - baseY;
            len = radius;
        }
        float mag = Math.min(1, len / radius);
        mag = mag <= dead ? 0 : (mag - dead) / (1 - dead);
        if (curve == 1) mag *= mag;
        else if (curve == 2) mag = (float) Math.sqrt(mag);
        tx = len > 0 ? dx / len * mag * sx : 0;
        ty = len > 0 ? dy / len * mag * sy : 0;
        if (ix) tx = -tx;
        if (iy) ty = -ty;
        if (restrict == 1 || restrict == 2) {
            float r = (float) Math.hypot(tx, ty),
                    step = (float) Math.PI / (restrict == 1 ? 2 : 4),
                    a = Math.round(Math.atan2(ty, tx) / step) * step;
            tx = (float) Math.cos(a) * r;
            ty = (float) Math.sin(a) * r;
        } else if (restrict == 3) ty = 0;
        else if (restrict == 4) tx = 0;
        float n = (float) Math.hypot(tx, ty);
        if (n > 1) {
            tx /= n;
            ty /= n;
        }
        tx *= maximum;
        ty *= maximum;
    }

    public boolean tick(long now, float smoothing) {
        float a =
                smoothing <= 0
                        ? 1
                        : (float) (1 - Math.exp(-Math.max(0, now - last) / (smoothing * 150.0)));
        last = now;
        x += (tx - x) * a;
        y += (ty - y) * a;
        if (Math.abs(tx - x) < .0001) x = tx;
        if (Math.abs(ty - y) < .0001) y = ty;
        return x != tx || y != ty;
    }

    public void release() {
        x = y = tx = ty = 0;
    }
}
