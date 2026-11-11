# r1-meta — metaprogramming / staging-semantics review

**Spec:** `design/vagent-actor-executor.md` v0.1 (dialectic-converged) **Lane:** does the executor
model respect the established staging/durability mechanics — code-as-cells, fragments, graft/learn
EDL, genesis replay, P15/P16. **Counts:** BLOCKING 2 · MAJOR 3 · MINOR 3 · NIT 1 (9 findings)

______________________________________________________________________

## Findings

### 1. BLOCKING — §2.1 "Weight storage" / §3.4 weight reads: a clause cell is *not* a junction

**Spec location:** `vagent-actor-executor.md:79-80` (§2.1 established mechanics), `:200` (§3.4
"(O(1) `+d.weight` edge reads)").

**Problem.** The claim "a clause cell *is* a junction: `setWeight`/`weightOf` hit the scalar
`+d.weight` edge arm — O(1) trailed edge read/write" is false on all three counts (mechanism, cost,
durability), and it contradicts the sibling spec's explicit V12 rule.

**Evidence.**

- `setWeight`'s junction arm (`meta.cpp:2045-2048`) fires only when the cell *already* carries a
  `+d.weight` edge. Nothing mints that edge on a clause: `graftPlan` (`meta.cpp:1396-1407`),
  `learnPlan` (`:1732-1741`), `learnHandler` (`:1780-1792`), `graftMethod`, and genesis (d)'s clause
  re-mint (`genesis.cpp:286-305`) all write only `+d.name`/`+d.grab`/`+d.spin` (plus a clause-side
  `priority` record for handlers). The only `+d.weight` mint in the tree is `sproutSynapse`'s
  junction (`meta.cpp:2070-2072`). For every clause, `setWeight` falls to the **clause arm**
  (`:2050-2059`): a `d.vars` record on `binds_.agentCell` keyed by the clause's `+d.name` verbatim.
  `weightOf` mirrors this (`:604` junction probe, `:609-617` keyed-record arm).
- `vagent-metaprogramming.md:2647-2650` says it verbatim: weights live "never directly on a clause
  cell: clauses are program-side, so a state-side `d.weight`/`d.vars` edge *to* one is the V12
  edge." The doc's claim inverts the sibling spec.
- **Cost:** `varsRecord` is a linear scan of the owner's `+d.vars` rank (`records.hpp:72-73`). Per
  candidate clause, `weightOf` costs a `+d.name` read plus an O(V) scan — and V grows monotonically
  (`meta.serial`, one record per weighted clause, `unlearn:*` and `fault:*` tombstones that are
  never reclaimed). §3.4's "O(1) edge reads" is actually O(C·V) per selection.
- **Durability of the claimed path:** writing `+d.weight` on a program-side clause shadows an
  image-band cell. `regenesisCycle` seeds `extraRoots` only with *state-side* persistent shadows —
  `!isEphemeral(ref) && ref > imageExtent` (`lineage.cpp:190-195`) — and the agent-root promote walk
  never reaches program structure (V12: no state→program edge exists to discover it through). The
  write would silently never promote; for learned clauses the entire cell is re-derived by patch
  replay anyway (`lineage.hpp:210-216`). The *keyed-record* channel is what is durable — proven
  end-to-end by `vagent_plasticity_test.cpp:1149-1161` (learned weight resolves post-genesis because
  the replayed clause carries the `patch:<n>` label verbatim).

**Fix.** Rewrite the §2.1 bullet and the §3.4 parenthetical to the record convention: a clause's
weight is a `d.vars` record on the agent cell keyed by the clause's `+d.name`; junctions are the
`d.synapse` arm only. Price the real cost — either hoist one label→weight index build per selection
(one rank scan, then O(1) lookups; keeps determinism via a `std::map` or sorted vector) or state
O(C·V) in §4.2's fuel accounting, including the tombstone-driven growth of V. Add the
name-less-clause refusal (`meta.cpp:2061-2063`) as an M0 labeling requirement: every clause M0
lowers must carry `+d.name` or its weight channel refuses.

### 2. BLOCKING — §3.5 bound body identity: `reify:` labels are not stable across genesis replay

**Spec location:** `vagent-actor-executor.md:216-223` ("Suspension records the bound body identity …
the head's `+d.name` label for installed copies (`patch:<n>`/`reify:` labels), the persistent ref
int for image-original op targets").

**Problem.** The recorded identity does not survive checkpoint → resume → genesis (d) replay for
exactly the case it must — a learned or body-patched clause. Every such suspended intention
false-positives a `body_replaced` fault at its first resume after a checkpoint.

**Evidence.**

- Installed copies are labeled `reify:<src>:<serial>` drawn from the durable `meta.serial` counter
  (`meta.cpp:1101-1105`). Genesis (d) re-enacts each patch by re-reifying fresh install copies —
  "each install copy's own `reify:<src>:<serial>` still draws" (`genesis.hpp:30-33`;
  `genesis.cpp:247-262` `reifyNamed` → `meta.reify`). The counter is monotone and durable
  (`meta.cpp:339-364`), so the post-replay `+d.spin` target is `reify:X:38`, never the recorded
  `reify:X:7`. Resume compares labels → mismatch → `body_replaced` on a body that was *not*
  replaced.
- `patch:<n>` never labels a `+d.spin` target: it labels the *clause* (`learnPlan`
  `meta.cpp:1733-1734`; verbatim re-mint at replay, `genesis.cpp:287-292`). The spec conflates
  clause label with attach-head label; the clause label alone cannot detect `learn_body` (same
  clause, new body), so it cannot be the key either.
- The persistent-int arm is correct for image-original targets: promote identity-maps base cells
  (`arena_manifold.cpp:1901-1904`), the image band replays verbatim, and P16 never re-kinds a
  non-ephemeral referent (`p16.cpp:104-108`). The recorded *data* (text/int in the intention's
  `d.vars`) is durable — the failure is key stability, not storage.

**Fix.** Record the channel genesis itself uses: the **registered head's recorded name** — the name
each patch's `+d.patch-head` chain carries and replay re-resolves via `codeNamed`
(`genesis.cpp:243-256`), reachable from the install copy through its `+d.entry` delegation to the
source head (`meta.cpp:878-882`). Concretely: record `{clause label, source-head name}`
(equivalently, the `reify:<src>` segment); on resume resolve the current `+d.spin` target's
source-head label and compare. Keep the persistent-int arm for image-side targets. While here: the
compare should gate the *first* continuation too — a repoint between adoption and the body's first
segment currently faults only at the next resume, after the new body has already run once on old
bindings.

### 3. MAJOR — §3.8: the "no-marks rule is satisfied by construction" claim is wrong under m0

**Spec location:** `vagent-actor-executor.md:253-255` ("the executor runs the `MetaRuntime` verb
synchronously between segments (cursor quiescent — where `mintMetaBindSet`'s no-marks rule is
satisfied by construction)").

**Problem.** `m0` is outstanding for the entire turn (§3.2 `:147` → `:157`), so
`arena.outstandingMarks() != 0` holds at every host step — the no-marks rule is *not* satisfied by
construction. `mintMetaBindSet` itself is never invoked by verbs (it runs once at genesis (c),
mark-free). What actually makes the seam safe is different, and one refusal class follows that the
doc never names.

**Evidence.**

- All verb writes are ordinary trailed arena ops — "no session verb needs a marks guard of its own"
  (`meta.cpp:1113-1121`). That, not quiescence, is what makes `learn_*`/`graft_*` legal under m0: a
  failed turn's `release(m0)` undoes installs, patch records and serial draws atomically.
- The live mark-window constraint is *dimension mints*: `reify`'s `resolveDimension` refuses an
  unfiled name under live marks (`meta.cpp:818-825`), because `arenaDims_` is untrailed. So a
  `learn_*` whose fragment recipe names a dim never filed refuses mid-turn with
  `reify: dimension '<n>' is not filed and cannot be minted under live marks` — a refusal class
  §3.8's "infallible-or-refusing" framing should enumerate, since §9.2/§9.3's handler-driven
  `learn_*` exercises go through exactly this path. (In practice recipes name only compile-time
  dims, which the emission filing list covers — `compiler.cpp:769-772`, `lineage.hpp:168-176` — but
  the constraint must be stated, not discovered.)

**Fix.** Correct the parenthetical: verbs run at statement boundaries because their writes are
trailed under m0, and `mintMetaBindSet` is irrelevant to them. State the dim rule: mid-turn meta
verbs may *resolve* dims but never *mint* them; a fragment recipe carrying an unfiled name is a
named refusal, not a crash.

### 4. MAJOR — §3.1/M0: the `home +d.agents` mint's stage and positional preconditions are unpinned

**Spec location:** `vagent-actor-executor.md:127-133` (`d.agents` adoption), `:360-365` (M0).

**Problem.** "A post-`E_ops` link op minted at genesis" names no genesis stage — and none of
runGenesis's (a)–(e) arms mints agent topology today. Worse, the design's two correctness premises
are positional invariants the doc never states.

**Evidence.**

- `runGenesis` takes `agentCell` as an **input** root (`genesis.cpp:505-507`; `genesis.hpp:79-82`
  "the durable-state root") — it must already exist in the folded lineage. The mint therefore
  belongs to the seed/emit path (`seedProgramImage`-time append, `lineage.cpp:92-106`) or a
  genesis-hook/first-turn write; the doc does not say which, and the choices differ on whether
  durability requires a promote first.
- The agent cell must land **above** `imageExtent`: `commitStateSide` returns `Clears` only for
  `ref > extent` (`arena_manifold.hpp:278-281`), and draining the last `d.patch` record unlinks the
  agent cell's *own* rank edge (`unlinkRank` pred-repair, `records.hpp:41-52`). If M0 mints the
  agent cell inside the image band, its edge-clears are Additive-only → stale rank heads →
  resurrected patches.
- `home` must land **at or below** `imageExtent`: §3.1's "Additive → durable-append-only, never
  `Clears`" premise inverts otherwise — if `home` is state-side, a `d.agents` rank unlink *would*
  commit and tombstone delisting is no longer forced. `home` is the lineage's `sliceGenesis` cell
  (`vortex_core.cpp:31-38`, `lineage.cpp:53-55`); whether it sits below the image band depends on
  the file-before-image seed order (`lineage.hpp:168-176`) — plausible but unpinned.
- Mark windows: no conflict found. `mintMetaBindSet` refuses under live marks (`meta.cpp:65-68`) but
  runs at (c), mark-free; the `+d.agents` link write is trailed and legal even under m0; the
  `d.agents`/`d.seen`/`d.evidence` dims must be pre-filed — correctly, all three are absent from
  `kAgentDimensionNames` today (`lineage.hpp:67-71`) and M0 already lists filing them.

**Fix.** Pin the mint site (recommend: the seed path, so the link is durable by construction before
any turn) and write both positional invariants as M0 acceptance conditions; add a genesis-level test
asserting `agentCell > imageExtent` and `home ≤ imageExtent`.

### 5. MAJOR — §3.5/§3.8: the segmented-body emission contract has two unnamed conventions

**Spec location:** `vagent-actor-executor.md:211-215` ("statement-segmented resumable graphs: entry
dispatches on the persisted `step` path into the segment spine"), `:251-256` (§3.8).

**Problem.** No structural conflict with `compileFragment`'s head shape — `+d.entry` → dispatch
preamble fits the existing `+d.entry`/`+d.values`/`+d.refs`/`+d.recipe` schema
(`fragment.hpp:71-121`), and no new descriptor *fields* are needed — but two load-bearing
conventions are unnamed, and one of them cannot use the obvious channels.

**Evidence.**

- The step-path channel cannot be a descriptor `+d.grab` parameter: `signatureOf`/`clauseFormals`
  compare parameter lists (`meta.cpp:1143-1165`, `:1366-1389`), so a synthetic `step` param fails
  every graft/learn signature check. It also cannot be an extra `runFragment` argument: the arity
  check refuses arg-count ≠ param-count (`meta.cpp:710-713`). What remains is a **reserved,
  non-parameter binding** on the cursor's `+d.vars` (the `$`-name convention,
  `vagent-agent-language.md:619-623` — e.g. `$step`, which M0's "`$`-reserved seeds" already
  gestures at) or an executor-side register — the doc never says.
- The step path is an ordinal **list** descending nested compound bodies
  (`vagent-agent-language.md:904`), with `while`/`for` re-entry re-testing the condition from frame
  variables (`:905-906`). §3.5's "segment spine" doesn't say whether dispatch is a flat segment
  index or a hierarchical ordinal walk — these emit different graphs.
- Each segment must tail `Return` for `validate`'s run-reachable rule (`meta.cpp:561-568`) and for
  `runFragment`'s `#Call` convention — worth stating since a suspending segment looks nothing like a
  normal Return-tail body.

**Fix.** Add an emission-contract paragraph to §3.5 or a new subsection: `+d.entry` → dispatch
preamble reading a named reserved binding (`$step`, ordinal-list encoding), per-segment
Return-tails, descriptor unchanged (say "no new descriptor fields" explicitly so M0 does not mint a
bogus parameter record).

### 6. MINOR — §3.4: enumeration freshness holds, but say "mid-turn" and price the scans

**Spec location:** `vagent-actor-executor.md:198-205`, `:73-74` (§2.1), `:369-382` (M3 test).

**Problem.** The freshness claim is true but under-specified on three points: whether learned
clauses join mid-turn, the per-candidate scan cost, and the unweighted default.

**Evidence.**

- `clausesOf` re-walks `rankAfter` and filters `unlearned(label)` per call (`meta.cpp:210-220`);
  `unlearn` unlinks the clause immediately *and* tombstones (`undoPatch`, `meta.cpp:1930-1933`,
  `:1995`). A handler-driven `learn_*` installs on the rank at once, so it joins at the next
  `clausesOf` — which can be *inside the same turn* (settle-loop `d.pending` dispatches and later
  selections re-enumerate under one m0). The doc's "next selection"/"next enumeration" is accurate;
  make "possibly within the same turn" explicit, since the M3 test wording ("affects the *next*
  selection") reads ambiguously.
- Freshness is not free: `unlearned()` and `weightOf` each scan the agent's `+d.vars` rank per
  clause (see finding 1) — O(C·V) per selection with V monotone in patch history; hoist one index
  build per selection.
- `weightOf` returns `nullopt` for an unweighted clause — the sort key for absent weights is never
  stated (0.0? absent-last?).
- Asymmetry worth a caveat: genesis `dropPatch` mints only `fault:<label>` (`genesis.cpp:219-228`)
  while `undoPatch`'s `dropDead` mints `fault:` *and* `unlearn:` (`meta.cpp:1917-1927`). In a
  pre-`Clears` lineage a resurrected tail clause would carry `fault:` but not `unlearn:` —
  unfiltered by `clausesOf`. One line noting the tombstone coverage is the `unlearn:` key only (or a
  symmetric code fix) suffices.

**Fix.** One sentence: learned clauses are visible to the next `clausesOf` including same-turn
settle selections; absent weight sorts as `0.0` (say which); tombstone filtering keys on
`unlearn:<label>`.

### 7. MINOR — §3.5/§3.4: the `learn_where` asymmetry is consistent — say it is deliberate

**Spec location:** `vagent-actor-executor.md:218-222`, `:199-200`, `:416-417` (M3 test).

**Problem.** The body-identity check watches `+d.spin` only; `learn_where` repoints `+d.grab`
(`meta.cpp:1821-1822`) and so escapes detection. Is the "affects the next selection" claim
consistent with an in-flight suspended intention? **Yes** — resume re-enters at `step` without
re-evaluating the test, and the `where`-produced bindings were snapshotted into the frame at
adoption (`vagent-agent-language.md:750`, `:898-900`), so continuing the old body is sound. The doc
just never says the asymmetry is deliberate, and the M3 test asserts the next-selection half without
pinning the no-fault half.

**Fix.** One line: a repointed `+d.grab` deliberately does not fault an in-flight intention — the
new test binds at the next selection, the running body keeps its adopted frame. Extend the M3 test
to assert a suspended intention on the repointed clause resumes *without* fault.

### 8. MINOR — d.patch replay-vs-live discipline for executor-minted patches is unstated

**Spec location:** `vagent-actor-executor.md` §3.2/§3.8 (absent everywhere; §8 milestones M1–M4).

**Problem.** Does handler-driven `learn_*` during a turn mint patches correctly? **Yes**, and the
doc never says so — an implementer could plausibly invent a "replay mode" flag or suppress patch
recording during genesis.

**Evidence.**

- `mintPatch`'s writes are all trailed under m0 (`meta.cpp:1646-1693`), so a turn's patch record
  commits or rolls back atomically with the turn — no half-written EDL. `nextSerial`'s
  mint-and-repoint is durable-monotone across committed history (`meta.cpp:356-360` explains why
  in-place value writes would rewind) and rollback-safe: a rolled-back turn's draws re-issue
  harmlessly since only committed serials exist.
- Genesis (d) "mints no patch records and draws no `patch:<n>` serial" (`genesis.cpp:5-7`,
  `:548-551`) — replay needs no cooperation from the executor; reify serials drawn during replay
  keep the counter monotone past all recorded ordinals.
- The P16 interplay is already safe: `onFlagged`'s marked-inventory enumeration runs at the flagged
  turn's *end* (`p16.cpp:11-17`), so a `learn_*` inside that turn lands in the sweep.

**Fix.** One paragraph in §3.8 or §4: `d.patch` records minted by handler bodies ride m0's
atomicity; rollback re-draws serials harmlessly; replay re-enacts without re-recording — the
executor must neither suppress nor special-case it.

### 9. NIT — citation slips in §2.1/§3.5

**Spec location:** `vagent-actor-executor.md:79-80`, `:219-220`.

- §2.1 cites `meta.cpp:2038-2064` for the `setWeight`/`weightOf` pair; `weightOf` is `:602-619` (the
  cited range covers only `setWeight`).
- §3.5 cites `meta.cpp:1850-1851` for "`graft_body`/`learn_body` repoint"; that range is `learnBody`
  only — `graftBody`'s repoint is `:1338-1347`.

______________________________________________________________________

## Lane summary

The executor model's *skeleton* respects the staging mechanics — patch replay, P16 rehydration,
trailed-mutation atomicity, tombstone filtering all hold up under the turn model. Two of its staging
claims are false as written: clause weights ride name-keyed agent-side `d.vars` records, not a
`+d.weight` junction edge (and the claimed edge is neither promotable nor spec-legal); and the §3.5
bound-body identity key must be the registered-head name, not the ephemeral reify label, or every
learned-plan intention fault-resumes after its first checkpoint. Neither defeat is structural — both
are corrected-claim fixes priced in the findings — but as written the spec misdescribes the
durability channel its own exit criteria depend on.

VERDICT: NOT CONVERGED
