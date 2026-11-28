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

public final class EditorGeometry {
    private static float clamp(float v, float low, float high) {
        return Math.max(low, Math.min(high, v));
    }

    public static float[] tools(
            float left,
            float top,
            float right,
            float bottom,
            float width,
            float height,
            float density) {
        float radius = Math.min(22 * density, Math.min(width, height) / 9),
                gap = 6 * density,
                cx = (left + right) / 2,
                cy = (top + bottom) / 2;
        float[][] candidates = new float[17][2];
        for (int i = 0; i < 8; i++) {
            double angle = Math.toRadians(-90 + i * 45);
            candidates[i][0] =
                    clamp(
                            cx + ((right - left) / 2 + radius + gap) * (float) Math.cos(angle),
                            radius + 2,
                            width - radius - 2);
            candidates[i][1] =
                    clamp(
                            cy + ((bottom - top) / 2 + radius + gap) * (float) Math.sin(angle),
                            radius + 2,
                            height - radius - 2);
        }
        int n = 8;
        for (int y = 0; y < 3; y++)
            for (int x = 0; x < 3; x++) {
                candidates[n][0] = radius + 2 + x * (width - 2 * radius - 4) / 2;
                candidates[n++][1] = radius + 2 + y * (height - 2 * radius - 4) / 2;
            }
        float[] result = new float[7];
        result[6] = radius;
        for (int tool = 0; tool < 3; tool++) {
            int best = -1;
            float bestScore = Float.MAX_VALUE;
            for (int i = 0; i < candidates.length; i++) {
                float x = candidates[i][0], y = candidates[i][1];
                boolean collides = false;
                for (int j = 0; j < tool; j++) {
                    float dx = x - result[2 * j], dy = y - result[2 * j + 1];
                    if (dx * dx + dy * dy < (2 * radius + 2) * (2 * radius + 2)) {
                        collides = true;
                        break;
                    }
                }
                if (collides) continue;
                float nx = clamp(x, left, right),
                        ny = clamp(y, top, bottom),
                        dx = x - nx,
                        dy = y - ny;
                float overlap = dx * dx + dy * dy < radius * radius ? 1000000 : 0;
                float preferred = (i - (tool == 0 ? 0 : tool == 1 ? 1 : 2) + 8) % 8;
                float score =
                        overlap
                                + (x - cx) * (x - cx)
                                + (y - cy) * (y - cy)
                                + preferred * radius * radius * .1f
                                + (i >= 8 ? radius * radius : 0);
                if (score < bestScore) {
                    bestScore = score;
                    best = i;
                }
            }
            if (best < 0)
                throw new IllegalStateException("Area insuficiente para ferramentas do editor");
            result[tool * 2] = candidates[best][0];
            result[tool * 2 + 1] = candidates[best][1];
        }
        return result;
    }
}
