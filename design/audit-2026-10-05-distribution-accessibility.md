# Distribution accessibility validation

This follows the [Walks audit](audit-2026-10-05-ux-walks.md) and
[coincident-link audit](audit-2026-10-05-ux-coincident-links.md). Distribution builds now require
their accessibility adapter. A successful tree dump alone does not establish native delivery or
completion of the J1–J6 journeys.

## Changes

Linux distributions build the pinned AccessKit C 0.22.3 source as a static library. Debian, Arch,
Fedora and Nix require it during the application build. Windows and macOS require the same pinned
release, bundle its runtime, and check the installed application after removing the original
build-time runtime directory. The Android application links the release's static adapter and ships
the matching Java delegate; its native library supports Android's 16 KiB page alignment.

The browser adapter sends the compiled C++ tree to semantic DOM controls and returns native browser
actions to the application. The standalone compiled adapter fixture passes browser accessibility
readback and action delivery. Full WebAssembly application builds still require ports for native
LibVLC media and Xuzz's RNP/libtorrent identity and networking dependencies; the fixture does not
establish that a complete browser distribution is usable.

Native Linux testing exposed two application defects: the document switcher advertised New Document
without delivering its action, and the plain editor toolbar retained a parent reference invalidated
by child insertion. Both are fixed. Form text and secret fields now advertise their implemented
replacement action. Password fixtures verify that published tree values remain masked.

Installed Xuzz readback exposed an empty-text-run conversion defect: omitting the empty value made
the native consumer panic. The adapter now preserves empty TextRun values, and the native client
reads a newly created empty editable document.

Android native testing also exposed incomplete shader extraction and a static-link collision: the
editor and Poppler both defined global `Page`, so editing destroyed an editor page through Poppler's
destructor. Android now extracts the packaged shader tree and generates SPIR-V through the existing
Make target. The editor page type is isolated in the library namespace; a regression includes both
actual headers to guard against the collision. Android diagnostics now reach logcat through the
named logging categories.

Package build validation also repaired missing mandatory media/database dependencies and standard
library compatibility gaps. The shared vector collector preserves borrowed ranges, single-pass
ranges, proxy references and move-only transformed values. A retained replay result avoids a
dangling reference in session initialization.

## Evidence and limits

| Target          | Evidence                                                                                                                                                 | Remaining validation                                                       |
| --------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------- |
| Linux native    | AT-SPI document text, New Document, Walks notes, many-to-many navigation, branches, references, exact Restore, restart/refusal and UI reopening checks   | Complete screen-reader journeys                                            |
| Debian package  | Actual package build/install; installed GL/Vulkan rendering and native client checks                                                                     | Clean complete local Actions invocation after recipe repairs               |
| Arch / Fedora   | Actual packages build/install; installed GL/Vulkan rendering, text/action readback and Walks notes                                                       | Clean complete local Actions invocation; full screen-reader journeys       |
| Nix             | Full application and AccessKit derivations build; installed GL/Vulkan, native text/action and Walks notes with matching runtime closure                  | Clean complete local Actions invocation; full screen-reader journeys       |
| Android         | ARM64/x86_64 APKs; matching delegate and shaders verified; API 28/30 native text/focus/click/edit round trips; API 28 Save Document and saved-path label | ARM64 device and screen-reader journeys; native Xuzz remains separate      |
| Browser adapter | Compiled C++ tree through Chromium accessibility; native actions, Unicode, modal focus, secrets, options and stale actions                               | Full applications blocked on media and networking ports                    |
| macOS           | Official VLC SDK headers, real Mach-O dependency closure and relocation; native AX client added                                                          | Native runner access, application execution, signing and trusted AX checks |
| Windows         | Pinned RNP helper builds with selected backend; UIA client and PowerShell syntax checked                                                                 | Native runner access, MinGW package build, relocation and UIA execution    |

Repository format/lint gates and focused Clang analyzer checks pass. The preceding full regression
runs passed 546 library tests, 1,218 engine tests, 59 Xuzz tests and 120 ZigZag tests. One video
interaction test skips under the headless driver and one existing engine test is disabled. Network
namespace suites were excluded from these runs. All 33 binary journey cases pass on GL, GLES and
software Vulkan. The complete fresh-frame image comparison passes using the existing backend
tolerances. Per-backend scratch configuration was isolated after reused settings produced
inconsistent footer layout.

Android API 30 initially timed out before provider attachment, then failed document readback while a
System UI ANR dialog held the active window. The native provider had attached and the editor
rendered behind that system dialog. Dismissing the dialog let the unchanged platform-client
assertions pass. These local emulators used software CPU emulation without KVM; failed attempts, the
focused-window diagnostic and the passing runs are retained in `/tmp/ux-a11y-android-*`.

Temporary validation logs use `/tmp/ux-a11y-*`, `/tmp/xuzz-web-a11y-*` and
`/tmp/xuzz-ranges-gcc13.log`. They are local evidence, not durable CI artifacts. Native clients use
isolated author data and configuration. Application runs remain hidden or headless; Linux uses a
private accessibility bus. Checks never write to the user's actual permascroll.

The installed `agy` executable identifies itself as an agent CLI and exposes no Actions runner
command. Installed `act` has Ubuntu container mappings and skips macOS/Windows jobs without native
runner mappings. Downloaded Linux images and an Android SDK are usable; a Linux container is not
evidence of native Windows UIA or macOS AX delivery. The remote packaging workflows are manually
disabled and were not enabled as part of this work.

The earlier native accessibility gap is narrowed to the exact controls exercised above. Complete
screen-reader journeys remain unverified. Remote authority discovery and unopened ZigZag formatting
inheritance remain separate UX findings.

## Installed-package follow-up

Native Walks testing found that AccessKit static text derives its accessible name from the value, so
application labels published only as labels were unreadable. The converter now preserves the label
and any displayed value in static text. Virtual focusable choices also advertise the native
Component interface required by AT-SPI Focus. The note field now publishes a real text run, caret
selection and its painted bounds.

`packaging/check-accessibility-linux.py <xuzz> opengl --walks` authors its link fixture through UI
input and default key bindings, then uses native AT-SPI controls to edit and save a note. It checks
readable preview/availability labels, native Focus, empty text readback, actual keyboard input and
the saved-note label. It captures three private-Xvfb frames and native trees. The client refreshes
its GI interface cache after the note changes from static text to a text input; this focused client
result does not establish that a screen reader handles that transition correctly. The pinned Linux
backend exposes no EditableText interface, so note replacement through native SetValue is not
claimed. Keyboard editing is verified.

The final installed Debian package passes that check, document text readback in both programs, New
Document activation and empty-document readback. Installed Xuzz renders with OpenGL and Vulkan from
outside the source tree. Evidence is retained under `/tmp/ux-native-walks-final3-evidence/`;
screenshots were inspected. Logs use `/tmp/ux-installed-native-walks-final3.log` and
`/tmp/ux-followup-debian-*.log`. The schema fallback and Walks persistence regressions pass (four
focused tests); the native converter and Walks overlay pass Clang static analysis.

Arch and Fedora repositories lack the required RNP package. Their recipes now build checksum-pinned
RNP 0.18.1 into a private library directory and retain its licenses. The shared helper also
preserves the Windows recipe's existing generator and OpenSSL selection. The Makefile now consumes
the full Xanadu pkg-config include flags, allowing private-prefix RNP headers to compile. Nix wraps
the canonical Xuzz executable so its aliases enter the runtime wrapper, and its installed-check
closure supplies matching Mesa, fonts, GI typelibs and a private accessibility bus on non-NixOS
hosts. A named owning schema fallback avoids a GCC 16 dangling-pointer diagnostic without changing
schema defaults.

The installed Debian `--navigation` check passes a two-document, two-left/three-right link fixture
authored through UI input. Both documents are preserved through the Save UI before link marking; the
temporary-document preservation dialog is accepted through its default keyboard action. Native
member actions and occurrence Focus preserve the source caret; native occurrence Click enters the
exact target range 8–13. Back and a different right occurrence create sibling visits. Native Walks
Focus previews Visit 2 while Visit 3 remains current; Reference and Restore return to Visit 2's
exact range. After a graceful Quit and restart with only the source document open, the three visits
and reference persist. Restore of the closed target displays a refusal and leaves the current visit
and visit count unchanged. Both documents retain identical operation/table hashes throughout,
including after restart.

The real Alt+Shift+W shortcut exposed another product defect: SDL delivered `W` as text after the
command consumed the key, replacing the target selection before the queued Walks overlay opened. The
input handler now discards the text associated with an executed Ctrl/Alt shortcut. A blocked
document binding leaves the modal's text input intact; the note check caught a preliminary filter
that dropped `r` from `native branch note` because its document binding was blocked. Android
keyboard input also caught loss of ordinary characters bound to bare camera commands; those commands
retain their existing text entry behavior. The native check verifies that opening Walks leaves the
target text unchanged. Earlier fixture attempts that left a Save dialog open or modified the target
version are failed attempts, not evidence of passing navigation. The client waits for Walks before
operating Close and retries transient native-tree removals while refreshing cached interfaces. Six
inspected frames, native trees and document hashes are retained under
`/tmp/ux-native-modified-debian-final/fixture/`; the final run log is
`/tmp/ux-shortcut-modified-debian-navigation.log`.

Fedora's Poppler stream constructor also disproved the version-based API gate. Constructor feature
detection preserves ownership with both raw-pointer and unique-pointer APIs; thirteen PDF and
text-source tests pass. Fedora's hardening/link step exposed missing position-independent code and
ignored CFLAGS on bundled seekable-Zstd C objects. The C build now applies both and tracks its own
flags so previously cached objects rebuild. This leaves unrelated C++ objects untouched when only C
flags change. Linux CI retains the native test frames, trees and application log as artifacts.

The Fedora package passes RPM hardening, rpath, debug-file, desktop and AppStream checks. Its
private RNP library is excluded from global Provides and the application's RNP Requires; RNP's
actual system-library dependencies remain in the generated metadata. Android's enhanced API 28
client also verifies the native Save Document action and exact saved-path label. A guest System UI
ANR required dismissing the observed dialog before that client could access the editor; the failed
attempt and passing result remain in `/tmp/ux-a11y-android-label-*`.

After the keyboard fix, all 546 library tests pass again and the input handler passes Clang static
analysis. Focused schema/Walks persistence, PDF/text-source and seekable-Zstd checks passed earlier
in this follow-up. Native platform-client checks cover the controls described here; they do not
establish completion of J1–J6 with a screen reader.

The final Debian, Arch, Fedora and Nix packages pass installed GL/Vulkan rendering, native document
text/actions, empty-document readback, Walks note editing and the two-by-three native navigation
check, including restart/refusal and unchanged document hashes. Their inspected private frames and
native trees are retained under `/tmp/ux-{native,walks}-modified-{debian,arch,fedora,nix}-final/`.
Logs use `/tmp/ux-shortcut-modified-*`. The Nix check runs with libraries from the same package
closure, rather than host Mesa/GI libraries. Both Android ABIs build again with the final input
handler and pass adapter/delegate/shader content checks.

Nix's formatter and CI's source-built `nix-linter` pass on the new validation closure and
application recipe. The Nix CI gate now includes that validation expression. The final input handler
again passes all 546 library tests and Clang static analysis. Earlier broader regression results
above remain separate from these focused follow-up checks.

The final API 28 Android run passes native text/focus/click, complete keyboard text input, Save, the
exact saved-path static notification and persisted edited bytes. The notification probe now observes
the native content-change event and reads its source while the transient toast is present; polling a
previously cached window root missed it. An attempted root-refresh probe also failed and was
removed. These are retained failed attempts, not passing evidence. The final result is in
`/tmp/ux-shortcut-modified-android-event-native.log`; both ABI builds and APK content checks use the
same prefix. The emulator was hidden and used disposable fixture content. Live announcement delivery
with TalkBack and ARM64 device execution remain unverified.

## Closed-target reopening follow-up

The installed Debian, Arch, Fedora and Nix packages now pass reopening the unavailable target
through the Open dialog. After the restart/refusal check, the native client closes Walks, presses
Ctrl+O, activates Document and the target's local-store option, then presses Tab and Enter to accept
the form. Enter on the choice itself opens its list; the first attempt stopped there and is retained
as a failed harness attempt, not a product failure.

Native Walks Focus then reports the target available. Restore selects the original bytes 8–13,
retains the reference and all three visits, and leaves Visit 2 current. Both visited stores'
operation and table hashes remain identical to the pre-navigation hashes. Four additional frame/tree
pairs cover the Open choice, reopened availability, exact Restore and unchanged visit count. Local
evidence is under `/tmp/ux-native-reopen-{debian,arch,fedora,nix}-final/fixture/`, with run logs
under `/tmp/ux-native-reopen-*.log`. This closes the focused reopening validation gap; it does not
certify all of J5 or the complete J1–J6 screen-reader journeys.

## Orca and status announcements

Orca 50.3 ran beside the installed Arch application on the same private Xvfb and accessibility bus.
Its actual speech-output log records document text, Walks visit names and current/reference state,
the closed-target warning, the Open dialog and the editable note field. It recognizes the note's
change from static text to an entry and echoes every character of `native branch note`. These
focused checks still use the native client's actions; they do not establish keyboard-only J1–J6.

The initial note run exposed a missing confirmation: Orca received the status name change but did
not announce `Note saved`, because the status was an ordinary label. Walks now marks its status as a
polite live region. The native note regression subscribes to AT-SPI's `object:announcement` and
requires the saved confirmation, retaining its message and priority in `walks-announcements.json`.
The rebuilt Arch, Debian, Fedora and Nix packages deliver `Note saved` with polite priority 1. A
separate Orca run confirms actual speech output after allowing queued character echo to settle; a
first attempt closed the reader too quickly and is retained as an inconclusive speech run.

Orca evidence is in `/tmp/ux-orca-navigation-service-fixture/`, `/tmp/ux-orca-note-fixture/` and
`/tmp/ux-orca-live-settled-fixture/`; the local probe driver is `/tmp/ux-orca-navigation-probe.py`.
Native confirmation evidence uses `/tmp/ux-orca-live-{arch,debian,fedora,nix}-final/fixture/` and
logs `/tmp/ux-orca-live-*.log`. The first two probe starts used buffered debug-file output as
readiness and timed out despite Orca starting. The final probe checks ownership of Orca's
session-bus service; those startup failures are harness failures, not application passes or
failures. Evidence collection excludes runtime sockets.

| Journey      | Step                                         | Affordance                                      | Outcome                                                 | Evidence                                              |
| ------------ | -------------------------------------------- | ----------------------------------------------- | ------------------------------------------------------- | ----------------------------------------------------- |
| J5 (focused) | Restore with the target closed after restart | Native Restore visit                            | Pass: refusal; three visits retained                    | `walks-restarted-refused.{png,json}`                  |
| J5 (focused) | Reopen the target                            | Ctrl+O, native Document/local option, Tab/Enter | Pass on Debian, Arch, Fedora and Nix                    | `target-open-dialog.{png,json}`                       |
| J5 (focused) | Restore the existing visit                   | Native Focus and Restore visit                  | Pass: bytes 8–13, reference and current visit retained  | `walks-reopened-{restored,visit-count}.{png,json}`    |
| J5 (focused) | Verify preservation                          | Closed-store dump and hash comparison           | Pass: visited stores unchanged                          | `reopened-document-hashes.json`, `*-ops.txt`          |
| Walks notes  | Enter and save a note                        | Native Focus, keyboard input, Save note         | Pass: text readback and polite native confirmation      | `walks-note-*.{png,json}`, `walks-announcements.json` |
| Walks notes  | Hear the confirmation                        | Orca attached to the private bus                | Pass on rebuilt Arch: actual `Note saved` speech output | `ux-orca-live-settled-fixture/orca-debug.log`         |

With the final live-status change, all four rebuilt Linux packages again pass the complete native
navigation/restart/refusal/reopening check. Final navigation evidence is under
`/tmp/ux-native-reopen-live-{debian,arch,fedora,nix}-final/fixture/`; both documents'
operation/table hashes remain unchanged. The Nix wrapper also passes installed GL/Vulkan rendering
and both programs' native text/action checks with its matching runtime closure. The application
build and Walks Clang analysis pass; repository formatting and lint gates pass. These focused checks
do not replace the earlier full unit regression or complete local Actions invocations.

## Document form follow-up

The [document accessibility audit](audit-2026-10-05-ux-document-accessibility.md) records the next
focused J1/J5 run, native form text and identity fixes, and a remaining Orca flat-review reading
gap. Its findings and package reruns are separate from the results above.
