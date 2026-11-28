package io.github.vitarecomps.frontend;

import java.io.*;
import java.nio.file.*;
import java.util.Arrays;

public class DiagnosticStoreTest {
    static void check(boolean value, String message) {
        if (!value) throw new AssertionError(message);
    }

    static void rejected(File root, String name) throws Exception {
        try {
            DiagnosticStore.resolve(root, name);
            throw new AssertionError("Accepted " + name);
        } catch (IOException expected) {
        }
    }

    public static void main(String[] args) throws Exception {
        Path root = Files.createTempDirectory("diagnostic-snapshots-");
        try {
            String bodyA =
                    "Report A\n" + "complete preview ".repeat(5000) + "\u00e7\u00e3o\nEND A\n";
            File a = DiagnosticStore.create(root.toFile(), bodyA);
            byte[] bytesA = Files.readAllBytes(a.toPath());
            File b = DiagnosticStore.create(root.toFile(), "Report B\n");
            check(!a.equals(b), "Reused filename");
            check(
                    Arrays.equals(
                            bytesA,
                            Files.readAllBytes(
                                    DiagnosticStore.resolve(root.toFile(), a.getName()).toPath())),
                    "A changed after B");
            check(Files.readString(a.toPath()).equals(bodyA), "Full UTF-8 snapshot differs");
            check(
                    Files.readString(DiagnosticStore.resolve(root.toFile(), b.getName()).toPath())
                            .equals("Report B\n"),
                    "Wrong B");
            for (String bad :
                    new String[] {
                        null,
                        "../" + a.getName(),
                        "other.txt",
                        "report-00000000-0000-0000-0000-000000000000.txt"
                    }) rejected(root.toFile(), bad);
            String directoryName = "report-11111111-1111-1111-1111-111111111111.txt";
            Files.createDirectory(root.resolve(directoryName));
            rejected(root.toFile(), directoryName);
            check(
                    Arrays.equals(
                            DiagnosticStore.columns(null), new String[] {"_display_name", "_size"}),
                    "Default projection");
            check(
                    Arrays.equals(
                            DiagnosticStore.columns(new String[] {"_size", "_display_name"}),
                            new String[] {"_size", "_display_name"}),
                    "Reordered projection");
            for (String col : new String[] {"_size", "_display_name"})
                check(
                        Arrays.equals(
                                DiagnosticStore.columns(new String[] {col}), new String[] {col}),
                        "Single column");
            check(DiagnosticStore.columns(new String[] {"unknown"}).length == 0, "Unknown column");
            System.out.println(
                    "PASS: unique immutable A/B snapshots, full UTF-8 body, exact file lookup,"
                        + " rejected paths, provider projection order");
        } finally {
            try (var files = Files.list(root)) {
                for (Path file : files.toList()) Files.delete(file);
            }
            Files.delete(root);
        }
    }
}
