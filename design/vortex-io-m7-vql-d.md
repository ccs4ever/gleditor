# M7-VQL-d: values (match, owned await, `let` sources, join)

Status: built, gate pending. Slice d of the [roadmap](vortex-io-m7-vql-roadmap.md) §6.1; the surface
is in [`vql-query-language.md`](vql-query-language.md) §4.13 "Values (M7-VQL-d)". No new world
effect, no core file, no on-disk format.

## 1. Scope and the rulings it rests on

a1 outcome `match`, a2 `await($h) as $r` as an owned record, a3 a script-local `let` as a write
source, offset or length, a4 `join`, a5 a payload-less outcome constructor. Owner decision Q1
(2026-11-26): `match` is exhaustive, `_` allowed, a missing tag is a diagnostic before anything
runs.

## 2. Design

**Match (a1).** A new top-level statement `IoMatchStatement` (`ast.hpp`), not a desugaring into
`if io_tag($r) = "..."`. The desugaring was refused: it would pass a capability, a request or a
receipt to the fallthrough silently (`io_tag` answers `Capability`), and exhaustiveness would have
no place to live. The statement lowers (`compile_io_effect.cpp`, `compileIoMatch`) to one relation,
`io_match_tag`, which answers the tag ordinal or refuses a non-outcome as `VQL_IO_MATCH`, then one
`Eq` and `BranchFalseCell` per arm. Binders are the relations the path functions already use
(`io_payload`, `io_error`, `io_detail`, `io_uncertain`, and a new `io_cause` for `partial`), each
bound as a stream that is empty when the outcome lacks the field, so `match` adds one relation and
no opcode. An arm's body is one return item (`parseReturnItem`, extracted from `parseReturnClause`)
compiled as a one-item `return`, so a path or parenthesised value means what it means anywhere.
Exhaustiveness is static, in the parser, against `io_match.hpp`: the operation of `$r` is tracked
from the action that bound it with `as` (a read: eleven tags; a write, append or sync: eight; both
include `Rejected`, the admission refusal), `await($h) as $r` inherits the operation of `$h`, and a
name the parser cannot place (a session variable) must name all twelve or use `_`. A handle, an open
capability or a capture receipt is refused by name with what to do instead. A later `let` or `for`
that rebinds the name forgets it, which can only make the check stricter. A failure the parser
cannot know (a session variable that is a capability) is the same code at run time.

**Owned await (a2).** `await($h) as $r` and `cancel($h) as $c` copy the outcome into a script-owned
record through `io_own_copy`, which is `ownOutcome` with `retireRequest = false`
(`script_runner.cpp`): the request stays admitted, so a second `await($h)` finds it, and the
script's end retires it with the rest. Refused: retiring the request as a named read does, because
`handleArm` would then find a freed handle (the behaviour change m7-vql-c r1 refused). The budget
rule is unchanged: a copy past the retention budget leaves the name on the service's outcome.

**`let` sources (a3).** The runner's pre-flight read only session variables, so a `let` of the same
script was refused as unbound. Names bound by an earlier top-level `let` (before any `for`) now skip
that check, and the compiler (`ioBoundCell`) narrows such a name to exactly one value with two
asserts whose text leads with `VQL_IO_EFFECT_ARGUMENT`; `failureOf` turns a VM failure that starts
with a registered `VQL_IO_*` code into that typed diagnostic. Without the narrowing an empty or
several-valued `let` would be written as nothing, or as its own name. The Submit hook resolves
computed slots to their value cell (a `let` slot holds a reference) and `sourceBytes` reads a
constant's value when it has no spooled text. Offset and length are checked by the same `number()`
the hook always used.

**`join` (a4).** `join(rows, separator)` lowers in `compile_path_functions.cpp` as a fold of the
text `+` (`VqlAdd`) over `VqlString` of each row, the first row outside the loop so no separator
precedes it, one text cell at the end, `""` for no rows. The roadmap's fallback (a host relation
provider) was not needed: measured, 10^4 rows join in well under a second and the nine join tests
run in 1.5 s. The bound is one slice past `maxJoinedTextBytes` at the end (default 1 MiB,
`limits.hpp`, equal to the default write bound and asserted so in `script_runner.cpp`), refused
`VQL_IO_BOUND`; it is forwarded from `QueryExecutionOptions` so a host with a larger write bound
raises both. The check is at the end, so intermediate text is bounded by the cycle budget times the
row size, not by the bound.

**Payload-less constructor (a5).** `OwnedOutcome::summary` mints a record of the tag, the error
code, its detail and the uncertain flag plus the `owned` mark: no payload, observation or Progress
rank. `CaptureHost::advance` falls back to it when the full copy of a failed step is refused or
throws, where it previously fell back to a generic refusal that hid whether the stop was certain.

## 3. Refused alternatives

| alternative                                  | why refused                                                                               |
| -------------------------------------------- | ----------------------------------------------------------------------------------------- |
| `match` as sugar over `io_tag` and `if`      | no home for exhaustiveness; a capability or receipt falls through silently                |
| A `match` arm that holds an action           | a match is pure and cannot park; one site per script run                                  |
| Matching an `open` name or a detached handle | not outcomes; refused by name with the path function or the `await` to use                |
| Retire the request on `await ... as`         | the second `await($h)` then fails on a freed handle                                       |
| A host relation provider for `join`          | the fold measured fast enough; the provider needs a runner and `join` is a pure aggregate |
| A new opcode for text length                 | the one-slice-past-the-bound read needs none                                              |
| Per-row bound check inside `join`            | doubles the cycles per row; the cycle budget already bounds the work                      |

## 4. Acceptance and tests

`tests/xuzz/vql_io_values_test.cpp`: grammar and static exhaustiveness (including the operation
tables and the refused names), compiled shape (one Submit, a match site that never waits), every tag
selecting its arm and binding its fields (twelve through detached cells, seven through the fake
backend), the stale and non-outcome refusals, exposure through binders, owned await and cancel with
a still-awaitable handle, `let` as source, offset, length and append source with typed refusals for
none, several and non-numeric values, `join` (separators, numbers, numeric-looking text, 10^4 rows
equal to the fold, the bound, a script write), and the summary constructor under arena pressure.
Leak pins of 10^4 runs: match, named await, join-and-write. Fail-before (run against the code with
the change reverted): exhaustiveness check off (2 tests fail), `io_own_copy` retiring (5 fail), the
`let` pre-flight exemption removed (1 fails).

## 5. As built

- **`io_cause`** is a new path function (the `partial` pattern's second binder). The cell store
  keeps the cause as a number; the runner names it.
- **`$why` of `partial` is the cause**, not an error code (a Partial has none).
- **`cancelled` and `timed_out` bind the uncertain flag**; `overflow`, `message_too_large`, `stale`,
  `conflict` and `unknown` bind nothing, and read the outcome through `$r`.
- **A script `let` is a session variable afterwards** (existing behaviour), and its cell lives until
  rebound: a script re-run 10^4 times with a `let` leaves one cell a run. This is the engine's
  session-variable lifetime, not this slice's; the join leak pin frees the old binding as the owner.
- **The capture-host fallback is unit-tested at the constructor** (fits where the copy is refused),
  not end to end: forcing a failed capture step under an allocation limit needs a harness the
  capture tests do not have. Slice f touches that code and should add it.
- **Stale names within a script**: a variable freed by this script's first admission is still read
  as the old cell until the script ends, as for every other reader; the typed refusal appears from
  the next script, which is what the existing tests pin.
- **`vquery`** needed no change; it constructs the runner with default `QueryExecutionOptions`, so a
  join over about ten thousand rows needs a higher `maxCycles` than the default 100000, which the
  slice-m constants audit should surface as a flag.
- Core files: none changed. No proposal.

## 6. Notes for slice e

Close is another action kind that names an outcome; `noteIoName` in the parser should record it (a
close's outcome is a write-class outcome) so `match` on it is exhaustive over eight tags. Closing a
host-bound name must leave it stale: reuse `reconcileBindings` and the stale marker rather than a
new state. `retireRequest = false` is the model for any action whose handle outlives its first
await.

## 7. Change history

| version | date       | change                                         |
| ------- | ---------- | ---------------------------------------------- |
| 0.1     | 2026-11-26 | Design and as-built for M7-VQL-d; gate pending |
