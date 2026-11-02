# Round 2 critique: Technical Writer (vagent-native-agent.md v0.2)

Tally: 0 BLOCKING, 3 MAJOR, 5 MINOR. Verdict: not yet converged; three internal inconsistencies to
fix, then approve.

Checks run: `mdformat --check` and `mdl -s .mdl_style.rb` both pass with no output. Spot-checked
citations are accurate (`runtime_abi.hpp:70` 15 dims, `mesh_epoch.hpp:17-24`,
`agent_executor.cpp:2246-2252/:2379-2386/:3271/:3895/:4789`, `provenance.cpp:240-254`,
`Makefile:633/:1334`, `system_docs.hpp:760-765`, `host_bindings.cpp:50-56`, `compiler.cpp:92/:411`,
`apps/vagent/main.cpp:11-14`, `agent_executor.hpp:334-341/:347-351/:410-413`, `ast.hpp`
`EAsk::within`/`withinMs`).

## Status of previous BLOCKING/MAJOR findings

- R1.1 (Grant `scope`): RESOLVED. `Terminal { token, term, rows, cols }` and
  `open_terminal(rows, cols)` define every field used in §10.4, §13.1.
- R1.2 (batch `Arg`): PARTLY. The batch is gone, but the §11.4 step grammar is still informal (see
  M3).
- R1.3 (`within` on ffi/batch): RESOLVED. Sequences are `effect def ... within` (§11.4, §11.5).
- R1.4 (`#Choice` in fragments): RESOLVED (moot, fragments removed).
- R1.5 (ops cannot declare the first test): RESOLVED. §6.2 binding family plus kind table. The seal
  command is still spelled two ways (m3).
- R1.6 (post-resume handle): PARTLY. `stale` now exists, but step 8 cannot fail (see M2).

## BLOCKING

None.

## MAJOR

### 1. (§12.3 vs §12.4, §10.3) Does a checkpoint revoke grants?

**Problem:** §12.3 says "Its native host may stay alive across a checkpoint, and the running agent
keeps its grants." §12.4 says on `checkpointed(pid)` the supervisor "revokes the agent's grants
(`agent_ended`), shuts down its host", and §10.3 defines `agent_ended` as including "checkpointed
for migration". P20.6 lists `checkpointed(pid)` as an unconditional observer event. A checkpoint
that is not a migration is therefore both revoking and non-revoking.

**Fix:** Pick one. Either checkpoint always ends grants and host (then delete the §12.3 sentence and
say the agent re-requests after any checkpoint), or split the observer into `checkpointed` (no
effect on grants) and `migrating`/`stopped`, and change §10.3/§12.4/P20.6 to match.

### 2. (§13.2 steps 8-9, §13.1) The stale-token assertion cannot fail as written

**Problem:** The §13.1 plan ends with `close_terminal(t)`, so `t` is already revoked (`released`)
before the checkpoint. Plan locals are not state that survives the plan, so a resumed copy has no
`t` to reuse; the step's `draw_greeting(t, ...)` "with the old `t`" has no source, and it would fail
`stale` even with no checkpoint. This is R1.6 again with the handle replaced by a token. Step 9
("one revocation (`released`)") also omits the `agent_ended` row that §12.4 produces on checkpoint.

**Fix:** Add a second test module variant (or a second plan) that opens a terminal, stores the
`Terminal` record in an agent `var`, goes quiescent, checkpoints and resumes. Assert `stale` there,
and state the exact ledger rows for each variant.

### 3. (§11.4 vs §11.2, N6 vs §7.1) Two definitions are not closed

(a) The example sequence's `finally endwin(), delscreen(sp), ...` names `delscreen`, which §11.2
declares only as a handle `destroy`, not as an `fn`; yet "every step is type-checked against its
declaration". The text says `finally` runs "in reverse order, the cleanup of every step that
succeeded" and also that it is an explicit list, and "a cleanup that names a step that never ran is
skipped" does not apply to `endwin()`, which names a function. `mvinnstr(y, x, n: 255)` mixes
positional and named arguments and `s: out cstring(n)` puts `out` before the type, while §11.2 says
modifiers follow the type and the parameter list also has a parameter named `out`. None of this
grammar is specified (R1.2 asked for it).

(b) N6 says `programRef` hashes four parts including "the binding-manifest bytes", but §7.1 stores
only `op_log`, §6.2 says the manifest ops are "recorded in the same log", and §7.2 says canonical
form is the recorded sequence. Are the manifest bytes a separate artifact member or a subsequence of
the log?

**Fix:** (a) Give the sequence syntax a small grammar block (step forms, named arguments, `finally`
semantics: explicit list, or reverse of successes, not both) and declare `delscreen` as an `fn`, or
let `destroy` serve as a legal `finally` step. Rename the `out` parameter in the example. (b) Say
whether `programRef` has three or four parts and where the manifest bytes live in `.vnprog`.

## MINOR

- §3 C3 cites "(§11.2)" for the `effect def ... within` rule and §6.3 item 3 cites §11.2 for
  synthesized names; the synthesis is in §10.1 and §11.4.
- §5 and §13.2 step 1 give two seal commands (`vagent native seal module.so`,
  `build/vagent-native-host --seal`); say that the first runs the second.
- §8.3's allow list omits calls the design itself needs under the `ENOSYS` default (`dup`, `fcntl`,
  `fstat`, `getpid`, `lseek`, `ioctl(TCGETS)`; `stdio.stream` is `dup` plus `fdopen`). Say the list
  is illustrative, or complete it.
- §6.2's bindings row says the manifest ops are "not part of the `Unit`" while §10.1 and §11.4 say
  they synthesize declarations into it; and N4/§16 say a native program expresses "exactly" what
  source can, which sequences and authorities exceed. Reword to "the Unit plus declarations
  synthesized from the manifest".
- The glossary and §6 call the calls `vn_*`, but the §6.1 members are `node`, `set_str`, ... on
  `vn_api`; `vagent.native.enabled` defaults false (§14.3) but §13.2 never sets it; the ledger
  "outcome" values (`granted`, ...) in §10.5 are undefined.
