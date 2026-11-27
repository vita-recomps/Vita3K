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

import android.content.*;
import android.database.*;
import android.net.Uri;
import android.os.*;
import android.provider.OpenableColumns;

import java.io.*;

public class DiagnosticProvider extends ContentProvider {
    public boolean onCreate() {
        return true;
    }

    public String getType(Uri u) {
        return "text/plain";
    }

    public ParcelFileDescriptor openFile(Uri u, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)
                || u.getLastPathSegment() == null
                || !u.getLastPathSegment().startsWith("SSD_"))
            throw new FileNotFoundException("Report unavailable");
        return ParcelFileDescriptor.open(
                ImportDiagnostics.reportFile(getContext()), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    public Cursor query(Uri u, String[] p, String sel, String[] args, String sort) {
        File f = ImportDiagnostics.reportFile(getContext());
        MatrixCursor c =
                new MatrixCursor(new String[] {OpenableColumns.DISPLAY_NAME, OpenableColumns.SIZE});
        c.addRow(new Object[] {u.getLastPathSegment(), f.length()});
        return c;
    }

    public Uri insert(Uri u, ContentValues v) {
        throw new UnsupportedOperationException();
    }

    public int delete(Uri u, String s, String[] a) {
        throw new UnsupportedOperationException();
    }

    public int update(Uri u, ContentValues v, String s, String[] a) {
        throw new UnsupportedOperationException();
    }
}
