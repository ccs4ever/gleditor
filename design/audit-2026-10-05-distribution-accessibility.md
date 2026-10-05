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

| Target          | Evidence                                                                                                                                                        | Remaining validation                                                               |
| --------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------- |
| Linux native    | Source-built static AccessKit; AT-SPI document text readback in gleditor and Xuzz; Xuzz New Document delivered through AT-SPI                                   | Native Walks and endpoint actions, complete journey execution with a screen reader |
| Debian package  | Local Actions package build and install; installed gleditor/Xuzz render with GL/Vulkan and pass native AT-SPI text/action checks                                | Clean end-to-end local Actions invocation after the recipe repairs                 |
| Arch / Fedora   | Package recipes require the pinned static binding and installed native client checks                                                                            | Package builds and installed client execution                                      |
| Nix             | Actual AccessKit derivation builds and installs its static binding                                                                                              | Full application derivation and installed client readback                          |
| Android         | gleditor ARM64/x86_64 APKs build; adapter, matching delegate and all shaders verified; API 28/30 native text/focus/click/edit round trips pass                  | ARM64 device execution and screen-reader journeys; native Xuzz remains separate    |
| Browser adapter | Emscripten-compiled C++ tree read through Chromium accessibility; actions return to C++; Unicode, modal focus, secret fields, options and stale actions checked | Full applications blocked on media and networking ports                            |
| macOS           | Official VLC SDK headers, real Mach-O dependency closure and relocation checked; native AX client added                                                         | Native execution, signing and trusted AX client execution                          |
| Windows         | Pinned RNP recipe builds with its selected backend on Linux; native UIA client added and PowerShell syntax checked                                              | MinGW package build, relocation and native UIA execution                           |

Repository format/lint gates and focused Clang analyzer checks pass. The clean regression runs pass
546 library tests, 1,218 engine tests, 59 Xuzz tests and 120 ZigZag tests. One video interaction
test skips under the headless driver and one existing engine test is disabled. Network namespace
suites were excluded from these runs. All 33 binary journey cases pass on GL, GLES and software
Vulkan. The complete fresh-frame image comparison passes using the existing backend tolerances.
Per-backend scratch configuration was isolated after reused settings produced inconsistent footer
layout.

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

The earlier native accessibility gap is narrowed to the exact controls exercised above. Walks,
many-to-many link endpoints and their full journey semantics still need native assistive technology
validation. Remote authority discovery and unopened ZigZag formatting inheritance remain separate UX
findings.
