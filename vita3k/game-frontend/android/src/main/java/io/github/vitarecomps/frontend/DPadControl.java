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

public final class DPadControl {
    public static int directions(float dx, float dy, float width) {
        float ax = Math.abs(dx), ay = Math.abs(dy);
        if (Math.hypot(dx, dy) < width * .12f) return 0;
        int bits = 0;
        if (ax > ay * .4142f) bits |= dx > 0 ? 0x20 : 0x80;
        if (ay > ax * .4142f) bits |= dy > 0 ? 0x40 : 0x10;
        return bits;
    }
}
