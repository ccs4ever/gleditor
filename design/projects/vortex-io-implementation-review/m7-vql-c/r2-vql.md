# Round 2 critique: VQL Expert (M7-VQL-c code)

Target `cb194c92` (round 1 fixes `dcce529b..cb194c92`). Tally: 0 BLOCKING, 0 MAJOR, 1 MINOR.

`build/vquery` was rebuilt at the target (`make vquery`) and driven headless (offscreen, dummy audio,
XDG under `/tmp/claude-1000/r2vql`), REPL by piped stdin and batch by `-e`/`-f`/`-o`.

## Status of round 1 findings

- **M1 (persist rule checked per line): RESOLVED.** Verified end to end:
  - REPL, derived text uncovered: `read as $r; let $t := io_payload($r); return 1`, then `return $t`,
    then `:save` prints `VQL_IO_PERSIST_NEEDS_CAPTURE`, and no store is written.
  - Same with the capture in the same script as the `let`: `effect { capture($r) } as $c; let $t := ...`,
    then `return $t`, then `:save` writes the store (covered). A partial capture `capture($r, 0, 5)` of
    an 11-byte read stays refused. Capturing only the first of two reads of the same file leaves the
    second read's exposure refused.
  - Scenario 2 (a later script that exposes nothing) is refused: the verdict is the session's.
  - Handle rows on a plain line: `return $src` then `:save` prints `VQL_IO_PERSIST_HANDLE`.
  - Batch: `--io-file src=F -o out -e 'return $src'` exits 1 with `VQL_IO_PERSIST_HANDLE`, no store;
    without `-o` it prints `io capability`. `-f` and `-e` with `return io_payload($r)` or a carried
    `$t` under `-o` are refused with `NEEDS_CAPTURE`, no store.
  - Consequence confirmed as documented: an exposure whose read was retired before a capture can never
    be covered, so a later `capture($r)` does not lift it (the REPL `:forget` decision is pending).
- **m1 (`:reconcile` half-typed decision): RESOLVED.** `parseReconcile` takes the whole line.
  `:reconcile`, `1`, `3 promote 5`, `3 promote 5 11 7`, `3 release x`, `3 promote 0x5 11`,
  `3 promote -1 2` and an overflowing length all print the usage line; `3 release`, `3 promote` and
  `3 promote 5 11` reach the coordinator (here "Nothing to reconcile", no journal).
- **m2 (script-local `let` as a write source): RESOLVED as scoped.** The message now names the
  limit; the deferral is recorded.
- **m3 (stale named `await` outcome): RESOLVED.** After a later admission the old name answers
  `VQL_IO_STALE_BINDING`, never `RelationNext expects ...`.
- **m4 (`io_bytes` of a carried record answers 0): RESOLVED.** `return io_bytes($q)` of a record whose
  script ended answers `VQL_IO_STALE_BINDING` (lease returned); a rebound name and a name in its own
  script still answer.

The typed `VQL_IO_STALE_BINDING` holds across REPL lines for `io_tag`, `io_bytes`, `io_payload`,
`io_captured` and `capture`; a name rebound with `as` is live again; a capability variable (`$src`)
is never marked stale; `let $q := $r` on a plain line is tracked and goes stale after the record is
retired (two admissions later, as designed).

## BLOCKING

None.

## MAJOR

None.

## MINOR

### m1. Plain-line stale refusals still carry the internal `RelationNext:` prefix

**Problem.** The disposition says the internal `RelationNext` text is gone, but on a plain REPL line
the refusal prints `Query failed: RelationNext: VQL_IO_STALE_BINDING: ...`. Only the script form
(`effect { capture($a) }`) prints the bare code. **Failure scenario.** `return io_tag($r)` after a
later script: the author sees an engine operator name before the code. **Evidence.** `staleRefusal`
returns `refusal.code + ": " + refusal.message` (`script_runner.cpp` staleRefusal) and the VM wraps
it as `RelationNext: ...`. **Fix.** Strip the operator prefix in the vquery plain-query error path, or
accept it and correct the disposition sentence. Polish.

## Notes

- Stale marker writes ("stale handle" as a write source, and `return $r` of a stale name printing
  that text) are the known Vortex-lane item; not repeated.
- The orphaned doc comment above `parseReconcile` in `apps/vquery/main.cpp` ("What the owner reads
  beside a pinned capture") belongs to `captureReasonName`; cosmetic.

VERDICT: CONVERGED
