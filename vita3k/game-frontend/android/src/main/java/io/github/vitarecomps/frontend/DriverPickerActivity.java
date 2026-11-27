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

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.widget.Toast;

public final class DriverPickerActivity extends Activity {
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (state == null) {
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.setType("*/*");
            i.addCategory(Intent.CATEGORY_OPENABLE);
            try {
                startActivityForResult(i, DriverImport.REQUEST);
            } catch (Exception e) {
                Toast.makeText(this, "Nao foi possivel abrir arquivos", Toast.LENGTH_LONG).show();
                finish();
            }
        }
    }

    protected void onActivityResult(int request, int code, Intent data) {
        super.onActivityResult(request, code, data);
        if (request == DriverImport.REQUEST) {
            if (code == RESULT_OK && data != null && data.getData() != null) {
                DriverImport.result(this, request, code, data);
            } else finish();
        }
    }
}
