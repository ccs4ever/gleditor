# Native eighteenth increment: failed pinned request restart plan

Status: plan only. Do not launch until the root freezes the eighteenth-increment candidate and
supplies its executable, library, dumper, and request-index decoder schema. This plan makes no
product edits and adds no native validation claim.

## Question

Does one UI-created pinned citation request keep the same durable request identity and full
immutable pin when its genuine no-connection failure is reopened after process restart? Does an
explicit user Retry reuse that request ID and pin, then return to the same no-connection failure
without creating a second request?

The run can establish local request durability and the visible no-connection outcome only. It cannot
establish remote content availability, signature rejection, or behavior during a real interrupted
transfer.

## Profile and build

Create a fresh eighteenth-increment run root. Copy only `data/` and `config/` from the
sixteenth-increment UI-authored workspace at `build/validation-sixteenth/native/`; create empty
`cache/` and `gpg/`. Do not copy seventeenth-increment candidate profiles, private keys, or inbox
state. Keep the same fresh profile across all three process launches; ensure only one Xuzz process
writes it at a time.

After root freeze, record the exact app, library, and dumper paths and SHA-256 values. Launch the
frozen binary under Xvfb X11 at 1600×1200×24 with `-noreset`, a private D-Bus session,
`SDL_VIDEODRIVER=x11`, `SDL_AUDIODRIVER=dummy`, `LIBGL_ALWAYS_SOFTWARE=1`, all three fresh XDG
paths, and the run-local `GNUPGHOME`. Preserve exact argv, environment, logs, frames, internal
`--dump-a11y` snapshots, exit codes, and source/inbox checkpoints. Do not use a mock transport,
`--test-publication-swarm`, or another flag to create a product capability.

## Process 1: create and fail one pinned request through the UI

1. Follow the already-proven native citation route from the seventeenth-increment run: use the
   default `Ctrl+Alt+Shift+L` link-and-response control, open the UI-authored package review, then
   choose its cited publication. Reuse the ordinary keyboard control sequence captured in the
   seventeenth-increment native candidate script; do not prepare a source file or submit the pin by
   API.
1. Capture review and download-form frames and the internal accessibility dump. Record the complete
   pin visible in the form: publisher, salt/publication name, carrier hash, sequence, version, and
   title. The known seed profile contains the same UI-authored citation used previously; verify the
   actual selected row before proceeding.
1. Submit the form through Return. Use scheduled ordinary Return inputs to refresh the status until
   the form reports the real configured-connection failure. Capture the failed form before closing.
   The user-visible status must say that no publication connection is configured and content
   availability was not checked. Do not treat `Queued` as completion or infer remote unavailability.
1. After the request reaches `Failed`, decode the eighteenth-increment versioned LMDB request index
   read-only. Record its request ID and every pin field. The decoder must follow the frozen schema;
   preserve original LMDB files only in the private run root, never the review archive.
1. Close the form and quit with the UI (`Escape`, then `Ctrl+Q`). Require application exit code 0.
   After process exit, capture `xudu-dump --section=ops --permascroll=...` for the source document
   and activity store; record package-visibility and request-index metadata. Do not claim the
   activity database is byte-identical if shutdown appends read-place metadata; compare decoded
   completed visits and current visit.

## Process 2: reopen the restored failure without retrying

1. Relaunch the exact same frozen app with the same XDG data/config/cache and GNUPGHOME, after
   confirming process 1 exited. Capture the initial screen and startup log. Before opening the
   request, decode the persisted inbox index read-only and compare request ID and full pin against
   the process-1 `Failed` checkpoint. Verify startup did not queue or duplicate the failed request.
1. Use the default `Ctrl+O` open-document control. The eighteenth-increment UI change is expected to
   list pinned downloads as
   `[Download] <title> / <version> #<sequence> [<full carrier hash>] — <phase>`. After freeze,
   inspect the real Ctrl+O row at 1440×1000 and confirm the full identity is visible; record any
   clipping. If the label is clipped at the normal size, capture an 800-pixel-wide follow-up and
   state exactly what is lost. Use ordinary Right input to select the unique failed item in the
   Document choice, then ordinary Return to open it. Capture the list, key sequence, reopened form,
   and internal accessibility dump. Confirm it is still `Failed` without a new Return/Refresh/Retry
   action and that the same full pin appears. Quit with `Escape`, then `Ctrl+Q`, and require exit 0.
   Only after process exit, decode the request index and compare the same ID and pin with process 1;
   this avoids reading a live LMDB writer.

## Process 3: explicitly retry the same failed request

1. Relaunch with the same profile. Use `Ctrl+O`, select the same failed download through ordinary
   keyboard input, and Return to reopen the status form. Capture the form and pin before retry.
1. Use ordinary Right to select `Retry download` from the initial Refresh action, then ordinary
   scheduled Return to submit it. Capture the immediate `Queued` status. Use ordinary Return to
   refresh after the no-connection result settles; capture the resulting failure and its internal
   accessibility dump.
1. Quit with the UI and require exit code 0. After shutdown, decode the index read-only. Require the
   same single request ID and exact pin, with `Failed` status and the real no-connection error. A
   second ID, changed pin, startup auto-retry, changed verification claim, or UI/state disagreement
   is a failure.
1. Compare source document operations, source store file hashes, package-visibility bytes, and
   decoded activity visits before and after all processes. Require source and package state
   unchanged and the completed-visit/current-visit counts unchanged. Preserve any permitted activity
   read-place metadata differences explicitly.

The inbox is expected to persist an `XPI1` snapshot in its existing directory using the frozen
strict bencode schema (the request record currently has 21 fields, including
`cancellationRequested`). After each process exits, run `/usr/bin/mdb_dump` against the inbox LMDB
in its default hexadecimal key/value mode and decode a copied dump with the schema-matched strict
bencode helper. Record the exact command, raw dump hash, decoded request ID, pin fields, and phase.
Do not inspect the live database with `mdb_dump`, use ctypes, or include the LMDB file in an
archive.

## Evidence and limits

Keep phase-separated frames (`process-1` create/fail; `process-2` restored failure; `process-3`
explicit retry queued/failure), the exact internal `--dump-a11y` snapshots, the ordinary key-event
log, launch argv and six XDG/headless variables, process exit files, all decoded request-index
checkpoints, original operation dumps, file hashes, and a concise observation table. Hash raw PPM
and RGB pixels; if PNGs are made, verify exact RGB equality. Exclude the copied data/config, cache,
GPG home, private keys, binaries, and live LMDB files from any archive.

The frozen seventeenth-build baseline used for the pre-fix restart comparison labels downloads only
by title and phase; its ID is hidden in the option value. The eighteenth-increment UI change adds
the pin title, version, sequence, and full carrier hash to the Open dialog row. Validate that
changed label in the frozen candidate at 1440×1000 before using it to reopen the request. The row
still does not show the publisher key; report that only if it prevents distinguishing the actual
requests in this one-request profile.

A native interrupted-transfer/cancel/restart variant remains unvalidated until a real publication
transport is configured through ordinary product UI. Without that, use focused engine tests only for
transport interruption and label the native variant `Not run`; do not substitute a mock UI service
or claim network behavior from the no-connection case.

## Outcome table (to fill after the frozen run)

| Step                      | User affordance                                                                   | Expected observation                                        | Evidence                                    | Outcome |
| ------------------------- | --------------------------------------------------------------------------------- | ----------------------------------------------------------- | ------------------------------------------- | ------- |
| Create pinned request     | Link-and-response review, cited publication, Return                               | One request for the visible exact pin                       | Process-1 frames/tree/index checkpoint      | Not run |
| Reach failure             | Status form, scheduled Return refresh                                             | Failed, no connection configured, content not checked       | Process-1 status frame/tree/log             | Not run |
| Restore without action    | Exit process 1; relaunch; `Ctrl+O`; select failed request; Return; exit process 2 | Same ID and pin; still failed; no automatic queue           | Process-2 frames/dump/index checkpoint      | Not run |
| Explicit retry            | Relaunch; `Ctrl+O`; select request; Right/Return; Refresh Return                  | Same ID and pin; Queued then the real no-connection failure | Process-3 frames/dump/index checkpoint      | Not run |
| Integrity                 | Read-only dumps and hashes                                                        | Source/package unchanged; visits unchanged                  | Before/after checkpoints                    | Not run |
| Interrupted real transfer | Real configured transport only                                                    | Cancel/restart behavior                                     | Not available in normal no-connection setup | Not run |
