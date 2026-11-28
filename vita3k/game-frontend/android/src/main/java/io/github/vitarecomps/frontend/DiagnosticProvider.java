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
import android.database.Cursor;
import android.database.MatrixCursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;

import java.io.*;

public class DiagnosticProvider extends ContentProvider {
    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public String getType(Uri uri) {
        return "text/plain";
    }

    private File report(Uri uri) throws FileNotFoundException {
        if (uri.getPathSegments().size() != 1)
            throw new FileNotFoundException("Invalid report path");
        try {
            return DiagnosticStore.resolve(
                    ImportDiagnostics.reportDirectory(getContext()), uri.getLastPathSegment());
        } catch (IOException error) {
            throw new FileNotFoundException("Report unavailable");
        }
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) throw new FileNotFoundException("Reports are read-only");
        return ParcelFileDescriptor.open(report(uri), ParcelFileDescriptor.MODE_READ_ONLY);
    }

    @Override
    public Cursor query(
            Uri uri, String[] projection, String selection, String[] args, String sort) {
        String[] columns = DiagnosticStore.columns(projection);
        MatrixCursor cursor = new MatrixCursor(columns);
        try {
            File file = report(uri);
            Object[] row = new Object[columns.length];
            for (int i = 0; i < columns.length; i++)
                row[i] =
                        OpenableColumns.DISPLAY_NAME.equals(columns[i])
                                ? file.getName()
                                : file.length();
            cursor.addRow(row);
        } catch (FileNotFoundException unavailable) {
            /* Unknown reports have no rows. */
        }
        return cursor;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException();
    }

    @Override
    public int delete(Uri uri, String selection, String[] args) {
        throw new UnsupportedOperationException();
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection, String[] args) {
        throw new UnsupportedOperationException();
    }
}
