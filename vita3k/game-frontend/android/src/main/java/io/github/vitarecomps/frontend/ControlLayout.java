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

public final class ControlLayout {
    private final ControlElement[] elements;
    private int count;

    public ControlLayout(int capacity) {
        if (capacity < 1) throw new IllegalArgumentException();
        elements = new ControlElement[capacity];
    }

    public int register(ControlElement element) {
        for (int i = 0; i < count; i++)
            if (elements[i].id.equals(element.id)) {
                if (elements[i].type != element.type)
                    throw new IllegalArgumentException("ID used by another control type");
                elements[i] = element;
                return i;
            }
        if (count == elements.length) throw new IllegalStateException("Control capacity reached");
        elements[count] = element;
        return count++;
    }

    public ControlElement get(int i) {
        if (i < 0 || i >= count) throw new IndexOutOfBoundsException();
        return elements[i];
    }

    public int size() {
        return count;
    }
}
