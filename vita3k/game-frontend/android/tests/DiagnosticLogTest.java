package io.github.vitarecomps.frontend;

import java.nio.file.*;

public class DiagnosticLogTest {
    public static void main(String[] args) throws Exception {
        Path f = Files.createTempFile("diagnostic-tail-", ".txt");
        try {
            String text = "header\nZRiF=secret\nlicence_key: secret\r\nnormal tail\n";
            Files.writeString(f, text);
            String out = DiagnosticLog.tail(f.toFile(), 192 * 1024);
            if (out.contains("secret") || !out.contains("normal tail"))
                throw new AssertionError("redaction");
            if (!Files.readString(f).equals(text)) throw new AssertionError("modified input");
            Files.writeString(f, "x".repeat(200000) + "\nLAST\n");
            out = DiagnosticLog.tail(f.toFile(), 100);
            if (!out.equals("LAST\n")) throw new AssertionError("tail boundary");
            Files.writeString(f, "x".repeat(200000));
            if (!DiagnosticLog.tail(f.toFile(), 100).equals("\n"))
                throw new AssertionError("partial line");
            Files.writeString(f, "discard\nFIRST\nLAST\n");
            if (!DiagnosticLog.tail(f.toFile(), 11).equals("FIRST\nLAST\n"))
                throw new AssertionError("lost complete line after cutoff newline");
            Files.writeString(f, "discard\r\nFIRST\r\nLAST\r\n");
            if (!DiagnosticLog.tail(f.toFile(), 13).equals("FIRST\r\nLAST\r\n"))
                throw new AssertionError("lost complete CRLF line");
            Files.writeString(f, "prefix\n\u00e7\u00e3o\nLAST\n");
            if (!DiagnosticLog.tail(f.toFile(), 9).equals("LAST\n"))
                throw new AssertionError("partial UTF-8 line");
            try {
                DiagnosticLog.tail(f.toFile(), 0);
                throw new AssertionError("accepted invalid limit");
            } catch (IllegalArgumentException expected) {
            }
            System.out.println(
                    "PASS: bounded tail, key-line redaction, input preserved, partial-line"
                            + " handling, invalid limit");
        } finally {
            Files.deleteIfExists(f);
        }
    }
}
