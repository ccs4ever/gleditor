# Initial native AT-SPI readiness and run report

This report records the initial `8a38fb2d…` candidate phase. The final polished candidate and its
native action-selection proof are recorded separately in
[native-atspi-validation.md](native-atspi-validation.md). This report replaces the earlier plan-only
status. The seventeenth increment's frozen UI and engine source received an isolated AccessKit build
and a real AT-SPI run. The test used a copied profile, a private D-Bus session and headless Xvfb; it
did not touch the user's live profile or a real display.

## Environment and bridge provenance

- `dbus-run-session` is available at `/usr/bin/dbus-run-session`.
- PyGObject imports `gi.repository.Atspi` with the installed Atspi 2.0 typelib.
- AT-SPI launch components are present at `/usr/lib/at-spi-bus-launcher` and
  `/usr/lib/at-spi2-registryd`.
- The selected `/tmp/ux-native-accesskit/accesskit-c-0.22.3` release tree has header SHA-256
  `c7e0dff6b97ba5793d5a092d6ef817d5a4ec5ffe409345f5927fc0da4b06cf61` and library SHA-256
  `b18e4be6155c46fb6eb101b1c7a2fb2d4bbc8bb948f8e4251285c373c0a75b01`. It is the versioned release
  tree, not the distinct locally built library at `/tmp/ux-accesskit-linux` whose source/build
  provenance is unknown.
- The run exposed the actual Xuzz application and its child form controls through AT-SPI. Some
  D-Bus/portal and dbind warnings appeared in the logs, but did not prevent registration or the
  checked operations. This is evidence for this environment and bridge build, not a claim that other
  desktop stacks have been tested.

## Build evidence

After the parent confirmed the engine and UI were stable, I reran the isolated target with
`GLEDITOR_NO_CCACHE=1`:

```sh
make -j"$(nproc)" OBJDIR=build/validation-seventeenth/a11y \
  GLEDITOR_ENABLE_A11Y=1 \
  ACCESSKIT_DIR=/tmp/ux-native-accesskit/accesskit-c-0.22.3 xuzz
```

The rerun exited 0. The frozen isolated app binary SHA-256 is
`8a38fb2d6a414da51ee074e267b17fc5461f5b7f32fd0ef719957520a456be86`; its linked A11y
`libgleditor.so.0` SHA-256 is `dbc8f98265ce47d0900b85a65550a4b5d0e3c43a8a4ba0be6d0055558f65dec1`.
App binaries can match the ordinary build while the linked library differs, so both hashes identify
this test artifact. The final rerun log and numeric exit code are in
`build/validation-seventeenth/a11y/evidence/final-build-rerun.{log,exit}`. A prior attempt with
ccache had malformed preprocessor output and failed in the compiler; disabling ccache resolved that
artifact without any source change. The successful final run used the same isolated `OBJDIR` and
versioned AccessKit tree.

## Native publication-form result

The native test copied only `data` and `config` from `build/validation-sixteenth/native` into
`build/validation-seventeenth/a11y/evidence/native-download-form/profile`. Cache and GPG directories
were newly created. It followed the ordinary keyboard route recorded in
`build/validation-seventeenth/native-baseline/evidence/05-citation-open-argv.txt`, stopping with the
Download publication form open. The temporary harness at `/tmp/ux-atspi-client-x11.py` changes the
repository helper's SDL video backend to X11 and removes `--no-present`, so the actual AccessKit
surface could register under the private headless Xvfb. It otherwise uses the native AT-SPI client
operations and plan.

The native operation plan passed. The snapshot at
`build/validation-seventeenth/a11y/evidence/native-download-form/run/native-download-form.json`
exposes:

- `Requested carrier`: `f8ff688c5680ab88c27e4297decb5e7640b86a31`.

- `Requested edition`: `document / a34 #2`.

- `Publisher package name`: `document`.

- `Verification`: `Not checked`.

- The form status says

  > Download failed: No publication connection is configured. Content availability has not been
  > checked.

  It separately says publisher enrollment is unchecked and that downloading never enters the
  publication.

- `Retry download` is present as an AT-SPI Action; `Open completed publication` is absent.

The requested carrier hash is the cited publication-manifest pin for edition 2. It is the `f8ff…`
publication seed under the authored `send-818…-2` outbox. `722911…` identifies a Link PACKAGE
carrier reviewed in the earlier package-review run; these hashes belong to different object domains,
so they must not be described as old and new editions of one carrier. The assertion plan, launch
record, action events, result, registry/application/session logs, and snapshot are all retained
under `build/validation-seventeenth/a11y/evidence/native-download-form/`.

This proves native AT-SPI discovery of the requested carrier, version/title, verification state,
truthful connection failure, and state-appropriate retry/open actions. It does not prove every
Action is operable: this plan waits for and inspects Retry but does not activate it, and it does not
invoke Open. A separate exploratory probe attempted `EditableText.set_text_contents` on a focused
text entry; AT-SPI returned `Unknown interface org.a11y.atspi.EditableText`. Keyboard focus and text
reads worked, and the form is composed of choice controls, but an AT-SPI text-setting path was not
demonstrated. The exact reason is not established for the selected release library, so this report
records the observed interface gap without attributing it to AccessKit. The route uses ordinary
application keyboard arguments to reach the citation and trigger Refresh once; it never chooses Open
or enters a downloaded publication.

## Persistence and scope

The copied profile's source default store and authored xanadoc stayed unchanged:
`xudu/xanadocs/default/{ops.nodes,store.tables}` and
`xudu/xanadocs/untitled-20261018-154726/{ops.nodes,store.tables}` have byte-identical before/after
SHA-256 values. The activity store's `xudu/activity/ops.nodes` changed during reader
navigation/shutdown. Decoding the activity operations before and after gives one visit with current
visit `1` in both states; the file difference did not add a visit. This test therefore does not
claim activity-store immutability. The user data/config sources were copied before launch, and the
cache/GPG directories were fresh.

The native run was under headless Xvfb at 1600×1200 with software OpenGL and dummy audio. It did not
include a visual screenshot for independent pixel review; that remains with the separate native
raster validation. Nor does this one failure path verify invalid signatures, incomplete referenced
payloads, or alternate desktop/AT-SPI implementations. The accessible tree did distinguish the three
statuses present here: exact requested pin, manifest verification (`Not checked`), and content
availability (`not checked` after the actual no-connection failure); publisher enrollment remained
an explicit unchecked statement rather than a positive trust signal.
