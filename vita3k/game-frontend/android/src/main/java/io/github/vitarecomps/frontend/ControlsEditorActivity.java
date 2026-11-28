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

import android.app.*;
import android.os.Bundle;
import android.view.*;
import android.widget.*;

public final class ControlsEditorActivity extends Activity {
    private OnScreenControls controls;

    protected void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setFlags(1024, 1024);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(0xff10151c);
        LinearLayout bar = new LinearLayout(this);
        bar.setGravity(Gravity.CENTER_VERTICAL);
        bar.setPadding(6, 0, 6, 0);
        bar.setBackgroundColor(0xee18202c);
        TextView help = new TextView(this);
        help.setText("Toque no botao para editar. Arraste para mover.");
        help.setTextColor(-1);
        help.setTextSize(13);
        bar.addView(help, new LinearLayout.LayoutParams(0, -2, 1));
        button(bar, "Botoes", () -> controls.editorButtonMenu());
        button(
                bar,
                "Salvar",
                () -> {
                    controls.saveEditorLayout();
                    finish();
                });
        button(bar, "Cancelar", () -> finish());
        button(
                bar,
                "Padrao",
                () ->
                        new AlertDialog.Builder(this)
                                .setMessage(
                                        "Restaurar o layout original? Toque Salvar para manter.")
                                .setPositiveButton(
                                        "Restaurar", (d, w) -> controls.resetEditorLayout())
                                .setNegativeButton("Cancelar", null)
                                .show());
        root.addView(bar, new LinearLayout.LayoutParams(-1, -2));
        controls = new OnScreenControls(this);
        controls.configure(true, 100, 60, new float[0], 1024, "LOCK");
        controls.beginLayoutEditor();
        root.addView(controls, new LinearLayout.LayoutParams(-1, 0, 1));
        setContentView(root);
    }

    private void button(LinearLayout row, String label, Runnable action) {
        Button b = new Button(this);
        b.setText(label);
        b.setTextSize(13);
        b.setOnClickListener(v -> action.run());
        row.addView(b);
    }
}
