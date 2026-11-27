# Diagnostic export host check

`DiagnosticLogTest` compiles the production `DiagnosticLog` directly with a regular JDK; it needs no Android stubs. From the repository root:

```sh
javac -d out/diagnostic-test vita3k/game-frontend/android/src/main/java/io/github/vitarecomps/frontend/DiagnosticLog.java vita3k/game-frontend/android/tests/DiagnosticLogTest.java
java -ea -cp out/diagnostic-test io.github.vitarecomps.frontend.DiagnosticLogTest
```

It checks bounded tail reads, matching key-line redaction (including CRLF), source preservation, truncation/partial-line handling and invalid limits. Matching key-line filtering is not a general secret sanitizer.

The other frontend Java files require Android SDK 35 and the pinned SDL Android sources, as in the normal Android build. Physical-device chooser/provider grants, native launch failure and other-game report locations remain manual checks. The all-feature local APK shared in ss-delta #29 has device feedback; this focused English/generalized report implementation has not separately completed that device matrix. It does not capture native SIGSEGV/tombstones or introduce periodic trace/counter collection.
