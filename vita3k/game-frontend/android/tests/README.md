# Diagnostic export checks

The host checks compile the production file helpers directly with a regular JDK, without Android stubs. From the repository root:

```sh
javac -encoding UTF-8 -d out/diagnostic-test vita3k/game-frontend/android/src/main/java/io/github/vitarecomps/frontend/DiagnosticLog.java vita3k/game-frontend/android/src/main/java/io/github/vitarecomps/frontend/DiagnosticStore.java vita3k/game-frontend/android/tests/DiagnosticLogTest.java vita3k/game-frontend/android/tests/DiagnosticStoreTest.java
java -ea -cp out/diagnostic-test io.github.vitarecomps.frontend.DiagnosticLogTest
java -ea -cp out/diagnostic-test io.github.vitarecomps.frontend.DiagnosticStoreTest
```

The tests cover bounded tail reads, key-line redaction including CRLF, unchanged inputs, cutoffs after a newline and inside a UTF-8 line, invalid limits, unique immutable A/B snapshots, full UTF-8 content longer than the old preview limit, exact filename resolution, rejected paths and missing files, and default/reordered/single-column provider projections. Unsupported projection columns are omitted. Matching key-line filtering is not a general secret sanitizer.

Each user-requested report is written once to a unique file in app-private `files/diagnostic-reports`. Its content URI identifies that exact file; later reports do not replace it. Reports are retained until app data is cleared or the app is uninstalled, so an attachment is not silently deleted while a recipient may still need it. Only explicit URI read grants expose individual reports. The provider rejects writes and never falls back to a different report.

Collection and report writing run on a dedicated worker. The complete collected text is passed to the preview; its Share button uses that same snapshot file without collecting again or reading files on the UI thread. Native launch failure notes are written on SDL's worker before its short-lived process exits; a main-thread caller queues note writing instead.

## Android device checks (pending)

Compile the frontend against Android SDK 35 and the pinned SDL Android sources as in the normal build. Host checks and SDK compilation do not validate Android chooser/provider grants or lifecycle behavior. On an actual Android device:

1. From setup, preview report A, then use its Share button to send/save the attachment through another app. Verify the complete preview, including its end, matches the saved UTF-8 text.
2. Keep A's URI in a test client (or keep the receiving app's attachment pending). Generate/view report B. Read A again through its original granted URI: A's bytes, name and size must be unchanged; B must have a different name/URI. A saved copy alone does not prove the original URI stayed stable.
3. Query an exported report URI with null projection, size only, name only, and size then name. Verify returned supported columns and values follow the requested order. Reject write access, invented filenames, traversal paths and missing reports.
4. After a genuine failed setup step, export from setup again and verify the relevant setup log or native-launch error is present. Also check the in-game Debug action where setup has succeeded. Logs are bounded tails of existing files, not a complete history.
5. Leave/rotate/recreate the activity during report preparation and confirm there is no stale dialog or crash. Report preparation must leave the UI responsive.

The all-feature local APK in ss-delta #29 has user feedback; this focused diagnostics PR has not separately completed this device matrix. It does not capture native SIGSEGV/tombstones or introduce periodic trace/performance counters.
