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

public final class TouchInputManager {
    private final int[] ids, owners, devices;
    private final float[] xs, ys;
    private int count;

    public TouchInputManager(int capacity) {
        ids = new int[capacity];
        owners = new int[capacity];
        devices = new int[capacity];
        xs = new float[capacity];
        ys = new float[capacity];
    }

    private int index(int id) {
        for (int i = 0; i < count; i++) if (ids[i] == id) return i;
        return -1;
    }

    public void put(int id, int owner) {
        if (index(id) >= 0) return;
        if (count == ids.length) throw new IllegalStateException("Pointer capacity reached");
        ids[count] = id;
        owners[count] = owner;
        devices[count] = 0;
        xs[count] = ys[count] = 0;
        count++;
    }

    public int get(int id, int fallback) {
        int i = index(id);
        return i < 0 ? fallback : owners[i];
    }

    public void remember(int id, int device, float x, float y) {
        int i = index(id);
        if (i >= 0) {
            devices[i] = device;
            xs[i] = x;
            ys[i] = y;
        }
    }

    public void delete(int id) {
        int i = index(id);
        if (i < 0) return;
        for (int j = i + 1; j < count; j++) {
            ids[j - 1] = ids[j];
            owners[j - 1] = owners[j];
            devices[j - 1] = devices[j];
            xs[j - 1] = xs[j];
            ys[j - 1] = ys[j];
        }
        count--;
    }

    public void clear() {
        count = 0;
    }

    public int size() {
        return count;
    }

    public int keyAt(int i) {
        return ids[i];
    }

    public int valueAt(int i) {
        return owners[i];
    }

    public void setValueAt(int i, int value) {
        owners[i] = value;
    }

    public int deviceAt(int i) {
        return devices[i];
    }

    public float xAt(int i) {
        return xs[i];
    }

    public float yAt(int i) {
        return ys[i];
    }
}
