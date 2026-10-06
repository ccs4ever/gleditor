# Xuzz document creation and form accessibility follow-up

This is a focused J1 and J5 follow-up using the real installed Linux application. Xuzz launches with
no arguments in a private Xvfb display and D-Bus session, with disposable configuration and
permascroll directories. The first document is created through the native New Document control; the
second uses the default Ctrl+N binding. Text, preservation, closing, reopening and continued editing
use real keyboard events. This is not a complete keyboard-only or J1–J6 screen-reader audit.

## Findings and fixes

The initial installed Arch run completed creation, typing, preservation and reopening, but the
Folder, Name and Custom path entries exposed no native Text interface. Orca could name these fields
without reading their contents. Form now supplies bounded Unicode text runs, character-based caret
positions and an empty run for an empty field. Secret fields retain masked values in every run, even
when their contents are revealed on screen.

After that fix, Orca read the preservation folder and name, but retained the previous dialog's
identity when the Open dialog appeared. A three-second pause between dialogs did not resolve the
stale title. Form now retains native node identities while editing one form and allocates fresh
identities on each opening. Actions addressed to fields from the previous form are refused.

The final attached Orca run announces `Preserve Temporary Xanadoc`, Folder and its actual value,
Name and `doc_7.xanadoc`, followed by the correct `Open Document or System Xanadoc` title and
`Custom path`. Character echo and the second document line are also present in its speech-output
log. These are generated speech records in a headless container, not a listening test with audio.

A further reading gap remains: after Ctrl+Home moves the native caret to zero, Orca's desktop
flat-review current-line command (KP_Up) says `document` instead of the first authored line. The
native client can read both the complete text and the first line correctly. The separate reader
probe is retained; it is not counted as a passing reading step. Investigate projected text-run and
character geometry in `src/a11y/documents.cpp` and the AccessKit bridge, then repeat actual Orca
line-reading and caret-navigation commands. The current document runs describe text but do not
supply individual run bounds or character positions; that is an investigation lead, not a proven
cause.

The captured restored-document frames also show a visual defect: the preserved filename
`doc_7.xanadoc` wraps below its tab and overlaps the first document line. Native text and resumed
input remain correct, but this visual step is not clean. Keep the label within the tab row in
`src/doc_switcher.cpp`, using a single-line layout or elision while retaining its full native name.
Evidence is `j5-reopened.png` and `j5-resumed.png` in the Arch document fixture.

## Journey evidence

| Journey      | Step                            | Affordance used                         | Outcome                                                  | Evidence                                                 |
| ------------ | ------------------------------- | --------------------------------------- | -------------------------------------------------------- | -------------------------------------------------------- |
| J1           | Start without prepared content  | Launch Xuzz with no arguments           | Pass: editable initial document                          | `j1-launch.{png,json}`                                   |
| J1           | Create twice                    | Native New Document action, then Ctrl+N | Pass: two additional documents                           | `j1-create-{control,keyboard}.{png,json}`                |
| J1           | Type two lines                  | Keyboard text and Return                | Pass: exact native text                                  | `j1-typed.{png,json}`                                    |
| J1           | Read first line and move caret  | Ctrl+Home, native Text readback         | Pass for native client                                   | `j1-read-first-line.{png,json}`                          |
| J1           | Read through Orca flat review   | KP_Up after Ctrl+Home                   | Fail: speaks document name                               | `ux-orca-documents-review-final/fixture/orca-debug.log`  |
| J1           | Preserve temporary document     | Ctrl+S, Tab, Return                     | Pass: Folder and Name expose their actual text           | `j1-preserve-{dialog,name}.{png,json}`                   |
| J5 (focused) | Close authored document         | Ctrl+W                                  | Pass: other documents remain open                        | `j5-closed.{png,json}`, `j5-closed-store/`               |
| J5 (focused) | Reopen using the displayed name | Ctrl+O, Tab, keyboard path, Return      | Pass: correct named dialog and readable path             | `j5-open-dialog.{png,json}`                              |
| J5 (focused) | Resume in the middle of text    | Reopen, then keyboard input             | Pass: caret 52 restored; eight characters inserted there | `j5-{reopened,resumed}.{png,json}`, `document-*-ops.txt` |

The permanent native regression is `packaging/check-accessibility-linux.py <xuzz> --documents`. It
captures frames, native names, interfaces, focus, editable state, text and caret positions. It
checks the destination folder, gets the preservation name from the UI, and uses that same name in
the Open dialog. It launches with no setup script or input file. Debian, Fedora, Arch and Nix
packaging jobs run this mode and retain its frame/tree evidence; the Nix wrapper accepts
`--documents` with the matching runtime closure.

The inspected Arch frames show the preservation fields, reopened document and mid-sentence caret.
Closed-store dumps in `/tmp/ux-form-arch-documents-persistence-final/fixture/` retain all 49
original operations exactly; seven appended insert operations contain precisely the eight characters
` resumed`, starting at byte 52. Input events can be batched, so characters and operation counts are
not assumed to match.

Initial reader evidence is under `/tmp/ux-orca-documents-first/fixture/`; the text-run-only attempt
is under `/tmp/ux-orca-documents-text-final/fixture/`. Final corrected dialog speech is under
`/tmp/ux-orca-documents-identity-final/fixture/`, with the flat-review attempt under
`/tmp/ux-orca-documents-review-final/fixture/`. The temporary probe drivers use the same private
bus/display as Orca and allow its queued speech to settle before terminating it.

## Scope and remaining validation

The first creation uses a native control action, so this run does not establish independently
finding every control using only the keyboard. The J5 portion covers one preserved document,
restored caret and continued typing within a running session. Slice focus, pouch items, selected
link, camera restoration and quitting/relaunching that combined context remain outside this run.
Choice-field selection announcements have not been certified. J2–J4 and J6 have not been repeated
with Orca in this follow-up. Native macOS AX and Windows UIA execution, Android TalkBack and ARM64
device execution remain unresolved as described in the distribution audit.

## Regression checks

All 549 library tests pass with the final Form changes. New cases cover Unicode values longer than
one text run, empty-field caret representation, stable identities during editing and rejection of
actions addressed to a previous form. The existing secret-value test now inspects child runs as well
as parent values. Form passes Clang static analysis.

The first final Nix navigation rerun timed out opening the target because the driver pressed Ctrl+O
immediately after queuing the native Walks Close action. The helper now waits for Walks to disappear
before sending that chord. The same installed Nix binary passes the repeated navigation check with
this wait; the failed attempt remains in `/tmp/ux-form-nix-navigation-final.log`. No product change
was made for this harness race.

Both Android ABIs rebuild with the final Form header and implementation and pass packaged native
adapter/Java delegate checks. These are build/content checks, not another device or TalkBack run.
The initial rebuild attempts used the runner's old default vcpkg checkout and an invalid inherited
cache setting. Using the prior build's `/root/vcpkg` checkout and the proper semicolon-separated
`VCPKG_BINARY_SOURCES` setting resolves both configuration failures. Logs are
`/tmp/ux-form-identity-android-build-verified.log` and
`/tmp/ux-form-identity-android-content-verified.log`; unsuccessful attempts remain separately named.

The rebuilt installed Debian, Fedora, Arch and Nix packages all pass native document creation,
preservation, closing, reopening and continued input; Walks note editing and polite confirmation;
and two-by-three navigation with Back branches, reference/current state, restart/refusal and
closed-target reopening. Visited-document operation/table hashes remain unchanged during navigation.
For the new document journey, all four closed-store dumps retain their original operations and
append exactly ` resumed` at the restored caret. Final document evidence is under
`/tmp/ux-form-{debian,fedora,nix}-documents-final/fixture/` and
`/tmp/ux-form-arch-documents-persistence-final/fixture/`. Final navigation evidence uses
`/tmp/ux-form-{arch,debian,nix}-navigation-settled-final/fixture/` and
`/tmp/ux-form-fedora-navigation-final/fixture/`; note evidence uses
`/tmp/ux-form-{arch,debian,fedora,nix}-walks-final/fixture/`. Their logs share these prefixes.

The Nix wrapper also passes both programs' installed OpenGL/Vulkan rendering and baseline native
text/action checks with its matching software graphics and AT-SPI closure. Repository formatting and
lint gates pass. These results cover the fixes and focused journeys described here; they do not
replace complete local Actions executions or certify the remaining reader/visual findings.
