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
