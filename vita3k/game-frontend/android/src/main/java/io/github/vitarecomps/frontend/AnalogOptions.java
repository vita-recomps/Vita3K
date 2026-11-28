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

import android.app.AlertDialog;
import android.content.Context;
import android.widget.*;

import org.json.*;

import java.util.*;

final class AnalogOptions {
    interface Commit {
        void accept(JSONObject settings);
    }

    static void show(Context context, boolean left, JSONObject original, Commit commit) {
        JSONObject draft;
        try {
            draft = new JSONObject(original.toString());
        } catch (Exception e) {
            draft = new JSONObject();
        }
        final JSONObject value = draft;
        LinearLayout body = new LinearLayout(context);
        body.setOrientation(1);
        int pad = (int) (16 * context.getResources().getDisplayMetrics().density);
        body.setPadding(pad, pad, pad, pad);
        ScrollView scroll = new ScrollView(context);
        scroll.addView(body);
        TextView note = new TextView(context);
        note.setText(
                "Original preserva a resposta anterior. Fixo usa o centro; flutuante nasce no toque"
                    + " dentro do controle e sua margem. Fora dessa area, o toque vai ao jogo."
                    + " Botoes e direcional tem prioridade. Salve o editor para manter.");
        body.addView(note);
        List<Runnable> save = new ArrayList<>();
        choice(
                context,
                body,
                value,
                save,
                "mode",
                "Modo",
                new String[] {"Original", "Fixo", "Flutuante"},
                0);
        number(
                context,
                body,
                value,
                save,
                "activationScale",
                "Area de ativacao (%; 100 = 1x)",
                150,
                100,
                300,
                100);
        number(
                context,
                body,
                value,
                save,
                "margin",
                "Margem de ativacao ao redor (dp)",
                24,
                0,
                80,
                1);
        number(context, body, value, save, "radius", "Raio (dp)", 60, 20, 160, 1);
        number(context, body, value, save, "sx", "Sensibilidade X (%)", 100, 10, 300, 100);
        number(context, body, value, save, "sy", "Sensibilidade Y (%)", 100, 10, 300, 100);
        number(context, body, value, save, "dead", "Zona morta (%)", 8, 0, 80, 100);
        number(context, body, value, save, "maximum", "Intensidade maxima (%)", 100, 10, 100, 100);
        number(context, body, value, save, "smoothing", "Suavizacao (%)", 0, 0, 100, 100);
        choice(
                context,
                body,
                value,
                save,
                "curve",
                "Curva",
                new String[] {"Linear", "Precisao", "Rapida"},
                0);
        choice(
                context,
                body,
                value,
                save,
                "restrict",
                "Direcoes",
                new String[] {"360 graus", "4 direcoes", "8 direcoes", "Horizontal", "Vertical"},
                0);
        check(context, body, value, save, "follow", "Base acompanha o dedo", false);
        check(context, body, value, save, "invertX", "Inverter X", false);
        check(context, body, value, save, "invertY", "Inverter Y", false);
        check(context, body, value, save, "base", "Mostrar base", true);
        check(context, body, value, save, "knob", "Mostrar alavanca", true);
        check(context, body, value, save, "idle", "Mostrar flutuante quando solto", false);
        new AlertDialog.Builder(context)
                .setTitle(left ? "Analogico esquerdo" : "Analogico direito")
                .setView(scroll)
                .setPositiveButton(
                        "Aplicar",
                        (d, w) -> {
                            for (Runnable r : save) r.run();
                            commit.accept(value);
                        })
                .setNegativeButton("Cancelar", null)
                .show();
    }

    static void put(JSONObject o, String k, Object v) {
        try {
            o.put(k, v);
        } catch (JSONException e) {
            throw new IllegalStateException(e);
        }
    }

    static void choice(
            Context c,
            LinearLayout b,
            JSONObject o,
            List<Runnable> save,
            String k,
            String name,
            String[] choices,
            int fallback) {
        TextView label = new TextView(c);
        label.setText(name);
        b.addView(label);
        Spinner s = new Spinner(c);
        s.setAdapter(
                new ArrayAdapter<String>(
                        c, android.R.layout.simple_spinner_dropdown_item, choices));
        s.setSelection(Math.max(0, Math.min(choices.length - 1, o.optInt(k, fallback))));
        b.addView(s);
        save.add(() -> put(o, k, s.getSelectedItemPosition()));
    }

    static void number(
            Context c,
            LinearLayout b,
            JSONObject o,
            List<Runnable> save,
            String k,
            String name,
            int fallback,
            int min,
            int max,
            float scale) {
        TextView text = new TextView(c);
        SeekBar s = new SeekBar(c);
        s.setMax(max - min);
        s.setProgress(
                Math.max(
                        0,
                        Math.min(
                                max - min,
                                (int) Math.round(o.optDouble(k, fallback / scale) * scale) - min)));
        text.setText(name + ": " + (s.getProgress() + min));
        b.addView(text);
        b.addView(s);
        s.setOnSeekBarChangeListener(
                new SeekBar.OnSeekBarChangeListener() {
                    public void onProgressChanged(SeekBar v, int p, boolean user) {
                        text.setText(name + ": " + (p + min));
                    }

                    public void onStartTrackingTouch(SeekBar v) {}

                    public void onStopTrackingTouch(SeekBar v) {}
                });
        save.add(() -> put(o, k, (s.getProgress() + min) / scale));
    }

    static void check(
            Context c,
            LinearLayout b,
            JSONObject o,
            List<Runnable> save,
            String k,
            String name,
            boolean fallback) {
        CheckBox x = new CheckBox(c);
        x.setText(name);
        x.setChecked(o.optBoolean(k, fallback));
        b.addView(x);
        save.add(() -> put(o, k, x.isChecked()));
    }
}
