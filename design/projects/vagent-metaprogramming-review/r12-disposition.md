# Round-12 disposition — orchestrator rulings on v1.11 findings

0 blocking, ~6 majors converging on **two mechanisms**, ~15 minors. One lane converged (ergo).
All r11 fixes verified — the majors are defects in v1.11's newest text (the ref-capture record
shape and the dedupe consequence), not reopened architecture.

## Majors — all adopted

**R12-A — the ref-capture `+d.grab → slot` edge collides with the op's own input wing**
(vortex + zigzag + architect — three lanes, same mechanism). First-position input slots carry
`-d.grab → opcode` backlinks (`vortex_core.cpp:384-390`); a second `+d.grab` link evicts the
first (`arena_manifold.cpp:1347-1377` displacement), decapitating the whole input wing at
emission and on every `reify` replay — the M14 single-occupancy argument the doc invoked against
`+d.ref` convicts `+d.grab` too. `+d.values`→referent has the same hole on shared/occupied
referents (zigzag). **Ruling:** the correlation edge uses a **dedicated correlation dim** —
a fresh `d.refFor`/`d.captured` dimension named at emission, or ref-record indirection through
the record cell itself (slot ↔ record ↔ referent positional pairing). `+d.grab`/`+d.values` are
barred as correlation edges; say which of the two replacements is adopted and restate the
record shape in §6.1's enumeration and genesis (e).

**R12-B — the dedupe-unlink resurrects declared clauses at replay** (meta + writer majors;
vortex + codebase minors — four lanes). The qualified-alias dedupe's unlink of a *declared*
clause is a program-side mutation never checkpointed; replay re-enacts no dedupe, so the
declared clause folds back at `1..E_ops` and coexists with `patch:<n>` — the "dead" name
resurrects and no patch records it for `unlearn`. The paragraph also self-contradicts
(:1174 "can hit a grafted clause" vs :1179 "unqualified alias always appends" — grafted
`+d.name`s are verbatim/unqualified, so unqualified aliases CAN verbatim-hit). **Ruling:**
state the dedupe's fate explicitly — either (i) the unlink is re-enacted at replay (the patch
record carries the superseded target and genesis (d) re-unlinks it), or (ii) dedupe applies
only to *learned* clauses (declared clauses are never unlinked — the learned `patch:<n>`
coexists and the shadowing rule is named); pick whichever composes with `plan_named`'s
verbatim walk and state it. Correct the self-contradiction: unqualified aliases verbatim-match
any clause `+d.name`, grafted or learned.

**R12-C — the kept-twin segment-apply surface is cross-referenced but never registered**
(codebase). `:1543-1547` claims it's "named as owed surface under §10.2"; §10.2 never names
it. **Ruling:** either register it in §10.2's owed-surface table (alongside the supply-side
injection and P4's wake) or drop the twin arm entirely — the `base()->store()` verify path
stands alone if the twin adds nothing.

**R12-D — genesis (a)'s comparand disagrees with §6.6** (writer). (a) :1992-1994 says
"non-scroll fields" only; §6.6 :1537 adds a `scrollKey` identity check while cross-referencing
"(a) states". **Ruling:** one comparand stated once — `scrollKey` belongs (it's identity, not
remap noise; note the `localScroll` guard codebase flags). Harmonize both sites to the same
canonical form.

## Minors — all adopted

- **`d.vars` counter needs the agent-cell pin** (architect): module `d.vars` is program-side and
  unpromoted — pin the serial to the agent cell's `d.vars` explicitly.
- **Replay `+d.name` re-mint vs re-link** (architect): a re-link evicts the record's own
  `+d.name`, dropping `patch:<n>` at second genesis — pin the transport as re-mint (fresh label
  cell, verbatim text), not re-link.
- **`(d) "no serial consumed" vs `reify` mint** (zigzag): §6.7's `reify:<src>:<serial>` mints
  on replayed install copies — scope "no serial" to `patch:<n>` specifically.
- **Shared `patch:<n>` `+d.name` label** (zigzag): M14 single-occupancy — two equal-text label
  cells, one per record.
- **`≤E_ops` agent-rooted walk under-claim** (vortex): `agent_cell()` itself program-side (the
  doc's exempt case :1608) makes image legs reachable — scope the claim.
- **Non-clause `learn_*` patch semantics** (codebase): `learn_where`/`learn_method` — does the
  record carry `+d.name→label`/serial? State per-verb.
- **Alias→clause join unstated** (meta + ergo): `clauses_of`+`patch:` can't discriminate by
  user alias — the name-keyed path is patch-rank alias field → `plan_named`; spell it.
- **"Same-named grafted clauses only" under-claim** (ergo): verbatim graft names can shadow
  *declared* clauses too — restate the surviving ambiguity correctly.
- **`learn_plan` dedupe scope** (ergo): global like `plan_named` or goal-rank-scoped — pin it
  (the `demo:Forager.travel` example depends on it).
- **`unlearn(patch_cell)` kind-dependence** (ergo nit): works accidentally via the shared
  `patch:<n>` label — state or refuse.
- **Citation nits**: `real.at`→`real.emplace` (`arena_manifold.cpp:1867-1868`);
  `store.cpp:3841`→`:3840`; `d.weight` miscast as agent rank dim; "publish `Store`" stale phrase
  :1891-1892; "the hydration" bare at :641; "ordinal read off the `d.patch` rank" at :1166 —
  apply all.

## Round status

v1.12 to follow. The majors are now pairwise-independent mechanisms inside two subsections
(§6.1 record shapes, §6.6 verify/genesis) — each fix round surfaces the next occupancy/
durability constraint the critics hadn't yet reached.
