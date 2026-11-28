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

public final class ControlElement {
    public enum Type {
        BUTTON,
        MENU,
        DPAD,
        LEFT_ANALOG,
        RIGHT_ANALOG
    }

    public final String id;
    public final Type type;
    public final int priority, commandMask;

    public ControlElement(String id, Type type, int priority, int commandMask) {
        if (id == null || !id.matches("[A-Za-z0-9._-]{1,80}") || type == null)
            throw new IllegalArgumentException("Invalid control ID/type");
        this.id = id;
        this.type = type;
        this.priority = priority;
        this.commandMask = commandMask;
    }
}
