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

/** Symbols and command-derived labels shared by saved and newly created controls. */
public final class ButtonAppearance {
    public static int symbol(int mask) {
        switch (mask) {
            case 0x1000:
                return 1;
            case 0x2000:
                return 2;
            case 0x4000:
                return 3;
            case 0x8000:
                return 4;
            case 0x10:
                return 6;
            case 0x20:
                return 7;
            case 0x40:
                return 8;
            case 0x80:
                return 9;
            default:
                return 0;
        }
    }

    public static String label(int mask) {
        int[] bits = {1, 8, 0x10, 0x20, 0x40, 0x80, 0x1000, 0x2000, 0x4000, 0x8000, 0x400, 0x800};
        String[] names = {
            "SELECT", "START", "\u2191", "\u2192", "\u2193", "\u2190", "\u25b3", "\u25cb", "\u00d7",
            "\u25a1", "L", "R"
        };
        StringBuilder out = new StringBuilder();
        for (int i = 0; i < bits.length; i++)
            if ((mask & bits[i]) != 0) {
                if (out.length() > 0) out.append('+');
                out.append(names[i]);
            }
        return out.length() == 0 ? "Botao" : out.toString();
    }
}
