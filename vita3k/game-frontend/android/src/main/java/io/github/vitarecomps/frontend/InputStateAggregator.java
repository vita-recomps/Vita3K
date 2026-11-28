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

public final class InputStateAggregator {
    private final ControlLayout layout;
    private final int[] buttons;
    private final float[] lx, ly, rx, ry;
    private final boolean[] left, right, seen;
    private final long[] leftOrder, rightOrder;
    private long sequence;
    public int outputButtons;
    public float outputLx, outputLy, outputRx, outputRy;

    public InputStateAggregator(int capacity) {
        layout = new ControlLayout(capacity);
        buttons = new int[capacity];
        lx = new float[capacity];
        ly = new float[capacity];
        rx = new float[capacity];
        ry = new float[capacity];
        left = new boolean[capacity];
        right = new boolean[capacity];
        seen = new boolean[capacity];
        leftOrder = new long[capacity];
        rightOrder = new long[capacity];
    }

    public int register(ControlElement element) {
        return layout.register(element);
    }

    public void beginFrame() {
        for (int i = 0; i < layout.size(); i++) seen[i] = false;
    }

    private static float safe(float v) {
        return Float.isFinite(v) ? Math.max(-1, Math.min(1, v)) : 0;
    }

    public void update(
            int slot,
            int mask,
            boolean ownsLeft,
            float x,
            float y,
            boolean ownsRight,
            float u,
            float v) {
        layout.get(slot);
        seen[slot] = true;
        buttons[slot] = mask;
        if (ownsLeft && !left[slot]) leftOrder[slot] = ++sequence;
        if (ownsRight && !right[slot]) rightOrder[slot] = ++sequence;
        left[slot] = ownsLeft;
        right[slot] = ownsRight;
        lx[slot] = safe(x);
        ly[slot] = safe(y);
        rx[slot] = safe(u);
        ry[slot] = safe(v);
    }

    public static int resolveOpposites(int mask) {
        if ((mask & 0x50) == 0x50) mask &= ~0x50;
        if ((mask & 0xa0) == 0xa0) mask &= ~0xa0;
        return mask;
    }

    public void finishFrame() {
        int mask = 0, ls = -1, rs = -1;
        long lo = -1, ro = -1;
        for (int i = 0; i < layout.size(); i++) {
            if (!seen[i]) {
                buttons[i] = 0;
                left[i] = right[i] = false;
            }
            mask |= buttons[i];
            if (left[i] && leftOrder[i] > lo) {
                lo = leftOrder[i];
                ls = i;
            }
            if (right[i] && rightOrder[i] > ro) {
                ro = rightOrder[i];
                rs = i;
            }
        }
        outputButtons = resolveOpposites(mask);
        outputLx = ls < 0 ? 0 : lx[ls];
        outputLy = ls < 0 ? 0 : ly[ls];
        outputRx = rs < 0 ? 0 : rx[rs];
        outputRy = rs < 0 ? 0 : ry[rs];
    }

    public void clear() {
        for (int i = 0; i < layout.size(); i++) {
            buttons[i] = 0;
            left[i] = right[i] = seen[i] = false;
        }
        outputButtons = 0;
        outputLx = outputLy = outputRx = outputRy = 0;
    }

    public int sourceCount() {
        return layout.size();
    }
}
