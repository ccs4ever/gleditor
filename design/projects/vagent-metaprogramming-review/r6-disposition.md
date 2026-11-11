# Round-6 disposition — orchestrator rulings on v1.5 findings

Five lanes converged in substance (zigzag, vortex, codebase, ergo CONVERGED; codebase/writer at
wording level): **0 blocking, 2 majors, ~15 minors + nits**. The ref-space ordering itself verified
mechanically sound (persistent refs are append-ordered spool indices, `manifold.hpp:295`;
`ephemeralBit` sorts above; shadows name base refs). The two majors are sequencing/writeback
precisions inside it.

## Majors

**R6-A — the `≤ extent` boundary presumes a layout nothing sequences (architect-1; zigzag-1 is the
same fix from the other side). Adopted.** The discriminator is only correct if the program image's
persistent refs occupy `0..E` below state refs *every incarnation*. `receiveValue` builds the state
manifold before genesis (`mesh_actor.cpp:260-271`); a flat append of program structure after state
lands program refs above E and silently inverts the test (V12 returns). **Ruling:** the genesis
sequence is explicit — (c) re-materializes the program image **first and ref-preserved**: program
cells are restored with the checkpoint's own numbering (the ops-spool indices *are* the persistent
refs — re-materializing from the same spool preserves ids), before any state overlay or
`learn`-replay mints a cell, so program refs land below the recorded extent by construction.
Program images arriving via `importValue` ingress (ephemeral twins) or a sibling manifold have no
program-side cells — the discriminator reports an empty set by design, not an error. Name the
extent's carrier: an adapter-side genesis field (the `programRef` bind record), recorded at load,
stable across genesis of the same checkpoint.

**R6-B — the shared `+d.entry` writeback writes entry ops into head-valued slots (meta-major).
Adopted.** Call-operand records need `int64(entryOp)` (`Call` executes `toInt(inputs[0])`,
`vortex_vm.cpp:854-856`); `run`-target records are **head-valued** — `run` derefs `+d.entry` *and*
`+d.values` (result/produced) at run time, so writing an op cell into the slot kills the fragment
on first `run`. **Ruling:** genesis splits writeback by `+d.name` shape — qualified-name records
(Call operands) write `int64(entryOp)`; label records (`src:`/`reify:`/`#label` run targets)
write `int64(head)`.

## Minor batch — all adopted

- **`patch:` arm has no referent** (writer R6-1): `patch:<n>` is a *clause-name* convention on
  clause `d.name`, not a head label — remove `patch:` from the rebind shapes.
- **`reflect`-born `run` arm is dead** (meta, ergo R6-F3, architect-m, vortex-m — 4 lanes): a
  reflect-born wrapper dies with the skeleton, is neither `d.code`-registered nor program-image —
  its name-record can never re-resolve. Drop the `reflect`-born arm from emitter auto-recording;
  `reflect`-born `run` targets in learnable fragments are closedness-refused.
- **`src:`/`reify:`/`patch:` as module names hijack the split** (meta, zigzag-n, ergo R6-F1):
  call records always write `module:`-qualified names; the shape test splits on first-`:` *and*
  requires the first segment to resolve as a module, else verbatim label.
- **`src:` labels for M13-cloned literals** (meta, ergo R6-F4): the label rule gains the
  origin-offset arm — a cloned literal's label is `src:<module>:<line>:<col>` of its *expansion
  site* plus a clone ordinal, or the doc names re-resolution ambiguous-by-design for spliced
  literals (pick: ordinal suffix `+n`, stated).
- **`home`/`+d.stack` misattribution** (codebase-1, meta, writer-n): `d.stack` hangs off cursor
  cells (`vortex_vm.cpp:159-160`), never `home`; `home` isn't a program-side anchor — the
  state-side root is the agent cell, and the walk's root-exemption clause is stated (a
  program-side `agent_cell()` cannot prune the durable set — the root itself is exempt).
- **`code_named` literal-head hole** (ergo R6-F2): state that program-side literal heads
  re-resolve through the program-image label→head lookup only — `code_named` stays `d.code`.
- **`ensureDimension` is find-or-mint** (codebase-2): at genesis dims resolve from base/cache;
  minting only absent names — fix the "re-mints" claim.
- **`plan_named` misdescription** (writer R6-2): a plan's qualified string lives on the clause's
  `+d.name`, not a `d.vars` symbol — the §6.7 bullet splits the two resolution paths.
- **`#label` uniqueness** (vortex-m): user `#label`s are uniqueness-checked at emission (the
  duplicate-label refusal already anchored at §6.7 applies to the lookup set); duplicate labels
  would misbind — state the enforcement point.
- **`run`-operand slot convention** (vortex-m): the slot holds the head by construction (the
  `run` ABI derefs `+d.entry`/`+d.values`) — state it once, consistent with R6-B's writeback.
- **`closed` two-arm clause** (vortex-m): a program-side capture link without a name-record is
  the refused class — one clause naming it on both arms.
- **Nit bundle**: `d.ref`/`d.mesh.relocation` phrasing, `writeValue` cite range, `manifold.hpp:154`
  off-by-one, glossary `closed` run-target arm, "left dead" vs "untouched", :1304-1305 phrasing
  on installed copies, per-record memo label freshness, "identifying label" for bare-op attach
  cells — apply all.

## Round status

v1.6 to follow. Majors halved again (8 → 5 → 2); the remaining surface is wording-level.
