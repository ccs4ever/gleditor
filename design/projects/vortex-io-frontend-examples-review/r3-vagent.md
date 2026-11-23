# R3 VAgent review

Target verified: `design/vortex-io-frontend-examples.md`, SHA-256
`043f09854ec8f11b62aaa2e5a43e9d8634e277eb708fd4f1cc7e07ffde466c30`.

References: `design/vortex-io-implementation-plan.md` §§3.2, 9–10; current VAgent
parser/executor/adapter behavior; prior reports `r1-vagent.md` and `r2-vagent.md` against the
earlier companion revisions.

## Findings

### MINOR — the sketch's goal header omits VAgent's existing goal kind

The sample now correctly declares `write(...): Unit`, uses `write(bytes)` as a statement, and says
the statement does not bind an I/O receipt. However, `goal SendReport(bytes: Bytes)` omits the
required `achieve`, `perform`, or `maintain` marker. The current parser requires one immediately
after the parameter list (`apps/common/xanadu/vagent/parser.cpp:524–537`). The passage clearly
labels the new declaration and `Bytes` type as proposed scaffolding; adding `perform` would still
make the unchanged surrounding VAgent syntax source-shaped and avoid implying a new goal grammar.

The implementation-plan §10 row still sketches `let result := Writer.write(bytes)`. With the
declared `Unit` return, that can only bind the language-level Unit value, not the I/O outcome. The
companion now says this plainly and removes the binding; update or annotate the plan row so its word
“result” cannot be read as the later `OutcomeCell`.

## Semantics confirmed

- **The R2 source/result issue is resolved.** The VAgent source no longer binds a `receipt` from a
  `Unit` effect. It explicitly says the statement stages the intent and is not the I/O receipt; the
  later owner event is the outcome consumer.
- **The proposed/current boundary is clear.** The companion says the full `OutcomeCell` event
  mapping is proposed host adapter glue, and states that current VAgent has no source outcome
  accessor. It explicitly identifies current `EffectFailed` behavior as an else-ladder wake with
  textual cause, unable to deliver the full rejection cell. This accurately distinguishes the future
  cell-preserving path from current `executor.cpp`/`adapter.cpp`.
- **Profile, provider, and durable staging are correctly stated.** The sample identifies `ActorTurn`
  staging followed by acknowledged durable admission, specifies the installed native skill and
  host-issued capability binding, and refuses failed-turn dispatch. This agrees with plan §9 and
  current installed-skill effect lookup.
- **Outcome preservation is complete for writes.** The admitted terminal variants and their retained
  fields match plan §3.2's write result set; `Rejected` is separate and covers pre-admission
  refusal. Unknown is not retried, and flush/close remain separate effects.
- **The scaffolding boundary is honest.** It does not claim current VAgent can inspect the event
  cell or run this async path, and it confines the new event fields to proposed adapter behavior.

## Verdict

**Approve the corrected VAgent outcome contract as an honest target example.** The Unit/no-receipt
source and explicit distinction from existing `EffectFailed` handling resolve R2's material
findings. Add `perform` to the goal header and clarify the plan's `let result` sketch as small
source-shape and cross-document corrections. No target or source files changed.
