# Round 7 critique — Programming Language Design & Ergonomics (v1.6)

Scope: v1.6 deltas only — verdicts on r6 findings R6-F1..F4 plus new ergonomic defects in the
changed regions (:1431-1493 shape-split + head-valued writeback, :1636-1670 clone ordinals +
`plan_named` + label uniqueness, :1085-1088 head-valued slots, :1444-1450 reflect-born refusal,
:1013 `code_named` row). Verified against `vagent-agent-language.md` v3.9 and the code
(`vortex_vm.cpp:854-856`, `vortex_core.cpp:119-127`).

## R6 verdicts

- **R6-F1 (shape predicate undefined, MINOR)** — **RESOLVED.** The predicate is now stated in
  full: the test "splits on the first `:` *and requires the first segment to resolve as a module*
  on `programRef`'s walk, else the whole string is a verbatim label" (:1475-1477); "call records
  always carry the `module:`-qualified spelling" (:1477-1478); label records are named as the
  `src:`/`reify:`/`#label` class (:1486-1488); and the failure mode is bounded — "degrades to a
  dropped patch, never a wrong bind" (:1478-1479). The ambiguity I flagged is closed twice over:
  user `#label`s cannot spell `:` (:1643-1644) so they can never leave the label channel, and the
  automatic prefixes are label-reserved (:1648-1649). `patch:` is out of the rebind shapes
  (:2488-2489). This is the proposed fix verbatim plus a reservation layer.
- **R6-F2 (`code_named` hole for literal heads, MINOR)** — **NOT RESOLVED in substance.** The
  ruled wording landed — `code_named` stays `d.code`-only (:1013, :2496-2497) and literal heads
  are routed to "the label→head structural walk of §6.6's name-record rebind" (:1657-1660) — but
  that walk is still not callable: `run`'s record path is emitter/genesis internals and no
  `std:meta` member exposes the program-image lookup (the read group ends at `label_of`,
  :1005-1014; U-M7 bars a user-side walk). §6.7's own contract — "the required one for handles
  received from another agent" (:1653-1654) + "across `resume` you re-resolve by label"
  (:1673-1674) — has no spelling for a foreign literal head. Residual MINOR: the doc's own
  disjointness argument (:1656-1658, "never `d.code`-registered, so no rank-order tie-break is
  ever needed") is the precondition for the one-line fix — `code_named` falls through to the
  program-image walk on a `d.code` miss.
- **R6-F3 (reflect-born `run` operands recorded but dead, MINOR)** — **RESOLVED.** :1447-1450: "a
  `reflect`-born operand is a session-side wrapper with no durable referent, so an opaque or
  `reflect`-born `run` target in a learnable fragment is a closedness refusal at emission" —
  proposed fix (a) verbatim; changelog :2489-2491 agrees.
- **R6-F4 (nested-literal `+o:c` arm missing from the label rule, MINOR)** — **PARTIALLY
  RESOLVED.** The adopted arm was the *M13-clone* `+n` ordinal (:1636-1638); the *nested*-literal
  arm — the actual finding — is still not in the rule: the base form is given as
  `src:<module>:<line>:<col>` (:1635) and `+o:c` appears only analogically ("the same `+`-suffix
  convention … §5.1"), never as a label arm. §5.1 already gives every nested position as
  `src:m:l:c+o:c` (:662-663, :926-927), so the rule still needs one clause: "a nested head's
  label is its full `src:` position — `+o:c` chained per level (a clone appends `+n` last)". Also
  narrow the trigger: "cloned by M13 expansion" should cover *any* multiply-emitted literal — a
  plain `splice f` at two sites of one `let f := code"…"` clones through `cloneExpr` too
  (:906-910) and shares the record/position.

## Delta checks that pass

- **The head-valued split is airtight.** `run`-operand slots "head-valued by construction"
  (:1084-1088) matches genesis's `writeValue(slot, int64(head))` with no `+d.entry` hop
  (:1489-1492) and `run`'s `+d.entry`+`+d.values` deref — R6-B implemented on both ends.
- **No wrong-type writeback is reachable.** A qualified name whose module stops resolving falls
  to the label channel, where no head can carry a `:`-bearing label (user labels can't spell
  `:`; `src:`/`reify:` are reserved) — so an entry-valued slot can never receive `int64(head)`;
  it drops instead (:1478-1479). The `d.code` fallback for `reify:`-labeled learned heads is
  order-safe because the two label spaces are disjoint by construction (:1491-1493, :1656-1658).
- **The `+d.name`-present discriminator** (:1463-1468, :968-971) reads cleanly with the split —
  name-record ⇒ no marker, no re-kind; the unified `+d.refs` schema diagram (:965-971) now shows
  the entryOp/head writeback inline.
- Changelog :2484-2497 accurately summarizes the deltas, including `patch:`'s removal from
  rebind shapes (the `:2461` mention is in the *1.5* entry — history, not a stale rule).

## New findings (v1.6 deltas)

### R7-F1. MINOR — `plan_named`/patch-target first-`:` splitting meets the `patch:<n>` clause-name convention with stored form underdetermined

:1645-1647 now routes `plan_named` **and patch target names** through first-`:`-with-module-
resolution, while two statements about what clause `+d.name` *stores* conflict: :1068-1069 has
the `name` parameter "held on a `d.name` label — learned ones are conventionally named
`patch:<n>`" (verbatim param), and :1669-1670/:1782 say "a plan's qualified name lives on its
clause's `+d.name`" (qualified form). Both cannot be literally true, and the readings diverge
in blast radius:

- *Verbatim-param storage*: `plan_named("patch:near_water")` works only while no module named
  `patch` exists — `patch` is **not** in the label-reserved set (:1648-1649 reserves only
  `src:`/`reify:`). One module declaration named `patch` (a plausible English-word module) makes
  every learned clause's recorded target resolve `patch` as the module qualifier → the learned
  clause (under its real module) is invisible → **all `learn`-recorded patches drop at genesis
  and `unlearn` misses** — an honest degradation, but the convention puts the whole EDL behind
  one token.
- *Qualified storage* (`+d.name` = `demo:Forager.patch:near_water`): resolution works, but then
  "conventionally named `patch:<n>`" is loose — `plan_named("patch:near_water")` cannot find
  the clause the user just named, and whether `plan_named` matches the full input or the
  post-module remainder against `+d.name` is unstated.

**Fix.** One clause: "`graft_*`/`learn_*` store the qualified `module:Agent.<name>` on the
clause's `+d.name` at mint" — recorded targets are then always `module:`-prefixed and `patch`
cannot hijack — or add `patch:` to the label-reserved prefixes beside `src:`/`reify:`.

### Nits (not findings)

- :1641-1642 "`#label`s are uniqueness-checked **at emission**" — the check's scope is unstated:
  if it runs per module/compile rather than over the merged `Program` (:908), cross-module
  duplicate labels reach the verbatim walk and bind first-found silently.
- :1636 "its *expansion site's* position" — call-site vs the literal's own position is ambiguous
  (`cloneExpr` retains the origin `loc`, :926). Verbatim matching works under either; provenance
  differs.
- :1650-1652 the durable `d.vars` counter is a state-side write `reify`'s table row (:1020)
  doesn't mention — legal for an impure verb, but hidden; `validate` reuses the same counter
  (:1728), making the omission systematic.
- :1645 lists `symbol_named` among first-`:`-splitting lookups though it takes `owner`/`name` as
  separate parameters — no single string to split; presumably means its reconstructed qualified
  form.
- :1478-1479 "unless a module literally takes that name" is dead text under :1648-1649's "no
  module can take those names" — harmless belt-and-suspenders, reads as a contradiction.
- `code_named`'s *required* spelling for learned heads is the full `reify:<src>:<serial>` — the
  serial is a durable-counter artifact a user cannot guess (two `reify`s per `learn_*`, order
  unstated); `label_of` recovers it but the doc never says that is the workflow. Bears on
  R6-F2's "required one" contract; pre-dates v1.6.

## Summary

v1.6 lands the shape predicate exactly as specified — first-`:` split gated on module
resolution, qualified-spelling convention, reserved prefixes, and a bounded drop-only failure
mode — and the reflect-born `run` arm is now a clean closedness refusal. Two r6 findings are
fully discharged; two have residuals: the `code_named` hole is documented but still uncallable
(R6-F2 — the doc's own disjointness note invites the fall-through fix), and the label rule
gained the clone `+n` arm but still lacks the nested `+o:c` arm (R6-F4). The one new defect is
the `patch:<n>`/first-`:`-split collision made possible by extending the split to patch target
names while clause-`+d.name` storage is stated two ways (R7-F1). All one-clause fixes.

VERDICT: CONVERGED — wording-level minors only
