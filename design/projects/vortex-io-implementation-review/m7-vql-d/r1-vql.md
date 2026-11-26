# Round 1 critique: VQL Expert (M7-VQL-d code)

Target: 301b9a8d (diff 0e6a672d..301b9a8d). Lane: `match`, owned `await ... as`, `let` sources, `join`,
`io_cause`, `VQL_IO_BOUND`, typed-failure mapping, diagnostics and docs.

Tally: BLOCKING 0, MAJOR 2, MINOR 4.

`build/vquery` in the worktree predated the slice (no `io_match_tag` in the binary); I rebuilt it
(`make vquery`) and ran every scenario below headless with XDG dirs under `/tmp/claude-1000`.

Checked and found sound (no finding): the VM-assert to `VQL_IO_*` mapping cannot be spoofed (every
Assert text is compiler-authored and the registered code is the prefix, `script_runner.cpp:1725`; the
only interpolated user text, `$name`, sits after the code); `join` cannot launder payload past the
persist gate (rows come from `io_payload`/binders, and both providers call `expose()` at
`script_runner.cpp:807`; an arm that returns the outcome is exposed by the scan at `:2077`);
`await ... as` twice and `cancel ... as` then `await` behave as designed (probed); binder streams are
empty when the field is absent (probed on an empty read); the parser's rebinding by `let`/`for`/a
later `as` forgets the operation (conservative); offsets from a `let` give typed refusals (negative,
fractional, text probed).

## BLOCKING

None.

## MAJOR

### M1. The read tag set omits `Conflict`, which the file read adapter produces: static exhaustiveness is false and a raced read silently matches no arm

**Problem.** `ioTagsOf(Read)` is the plan table's eleven tags with no `Conflict`
(`io_match.hpp:100-104`). The shipped file adapter ends a read in `Conflict` when the generation
moved around the transfer (`file_source_adapter.cpp:595-611`, "read raced a change", the
`!observedBefore == observedAfter` branch inside the `ReadBytes` path). So the table the parser
checks is not what a read can end with.

**Failure scenario.** (1) A script follows the recommended exhaustive form: all eleven read tags, no
`_`. The parser accepts it. A concurrent writer changes the file during the `pread`; the read ends
`Conflict`; `io_match_tag` answers the `Conflict` ordinal (`script_runner.cpp:1475`), every arm's
`Eq` fails, the match falls out at `#IO_MATCH_END` and the statement's result is the empty stream,
with no diagnostic. The slice's whole point (Q1: "a missing tag is a diagnostic before anything
runs", design §1, vql 4.13 "a forgotten `end_of_stream` is a diagnostic and not an empty result")
is defeated for exactly the tag that signals a data race. (2) The author who does know and writes a
`conflict` arm is refused: `match $r { complete($p) => $p, conflict => (1), _ => (2) }` on a read
fails at parse with `VQL_IO_MATCH: 'conflict' is not a tag a read can end with, so no arm for it
can ever run` (reproduced; the message is false). Writes include `Conflict` (generation CAS), so the
table is internally inconsistent about which operations race.

**Evidence.** `apps/common/xanadu/vql/io_match.hpp:100-104`;
`apps/common/xanadu/vql/parser.cpp:2618-2625` (the false refusal);
`apps/common/xanadu/vortex/file_source_adapter.cpp:595-611` (read yields `Conflict`);
`apps/common/xanadu/vql/compile_io_effect.cpp:421-503` (no runtime guard after the last arm);
doc claims at `design/vql-query-language.md` 4.13 ("a read can end with eleven tags") and
`design/vortex-io-m7-vql-d.md` §2.

**Fix.** Add `Conflict` to the Read set (twelve tags: a read then needs `_` or all twelve) and fix
the doc counts and tests. Independently of the table, emit a final fall-through guard after the last
arm of a match without `_` (an `Assert` with text `VQL_IO_MATCH: no arm names tag N`), so a tag the
static table missed is a typed refusal and not an empty result. Reconcile the plan §4 table with the
adapters (the plan table is what the code copied; the adapters moved).

### M2. A numeric `let` used as a write source writes zero bytes and reports `Complete`

**Problem.** The `let` narrowing (`ioBoundCell`) only checks "exactly one value"; `sourceBytes`
then takes `arena.textOf(source)`, and when empty falls back to `render()` but only keeps a
`std::string` alternative (`script_runner.cpp:1824-1832`). A number (or a computed `1 + 2`) has no
text and is not a string, so the bytes are empty. Nothing refuses it: the contract (design §2 a3,
vql 4.13) lists typed refusals for none, several and a non-number offset, and says nothing of a
non-text source, but "accepts a name ... as the bytes" is violated by writing nothing.

**Failure scenario.** `--io-write-root R`: `effect { open("R/o.txt") for write create } as $o;
let $t := 42; effect { let $s := submit($o, write_bytes(0, $t)); match $s { admitted($q) =>
await($q), refused($e) => $e } } as $w; return io_tag($w)` prints `Complete`, and `R/o.txt` is
empty (reproduced; same for `let $t := 1 + 2`; `let $t := join(1, ",")` writes `1`). A script
believing it persisted a computed value silently wrote an empty file; the tag it checks says
success.

**Evidence.** `apps/common/xanadu/vql/script_runner.cpp:1821-1832` (`sourceBytes`),
`apps/common/xanadu/vql/compile_io_effect.cpp:344-385` (`ioBoundCell` narrows count only, no type).

**Fix.** In `sourceBytes`, render a numeric value to its canonical text (as `VqlString` does, so it
agrees with `join`) or refuse it `VQL_IO_EFFECT_ARGUMENT` ("$t holds a number, not text"); never
fall through with empty bytes. Add a test for a numeric `let` source.

## MINOR

1. **`join` does quadratic copying and checks its bound only at the end.** Each row runs
   `joined = joined + piece` (`compile_path_functions.cpp:393-396`), so work is O(rows x final size)
   and intermediate text is unbounded by `maxJoinedTextBytes`; 10^4 rows of 100 KB (inside the default
   100000-cycle budget) would build and recopy about 1 GB, past a bound meant to be 1 MiB, before the
   refusal. The design prices this ("bounded by the cycle budget times the row size") but the standing
   rule is bounded memory; a per-iteration `Value` slice against the bound inside the loop (the design
   rejected it for cost) or a builder opcode would remove it. Not reproduced at scale.
1. **`vquery --io-write-bytes` does not raise `maxJoinedTextBytes`.** `apps/vquery/main.cpp:470,690`
   forwards the write bound but not the join bound; the design (§2 a4, §5 "vquery needed no change")
   says "a host with a larger write bound raises both". With `--io-write-bytes 4194304` a script
   cannot join 2 MiB to write it (`VQL_IO_BOUND`, fail-closed). The static_assert only ties the
   defaults. Forward it or correct §5.
1. **A `match` on an unbound name says "not an outcome".** `match $q { _ => (1) }` with `$q` never
   bound gives `VQL_IO_MATCH: a match is aimed at a value that is not an outcome`
   (`script_runner.cpp:1465-1473`), indistinguishable from a wrong-typed value. The `ioBoundCell`
   placeholder text `"$q"` is the evidence; name it ("$q is not bound").
1. **`complete` without parentheses is refused as arity 0 vs 1**
   (`parser.cpp` arity check; reproduced `'complete' binds 1 field(s) and the pattern names 0`).
   Design §4.13 reads "the other tags bind nothing" and examples show `end_of_stream` bare; the
   rule that a binding tag must be written with a pattern (`complete(_)`) is correct but not
   stated, and the error does not suggest `complete(_)`.

VERDICT: NOT CONVERGED
