# Round 6 critique — `design/vagent-metaprogramming.md` v1.5 — Metaprogramming Expert lane

Scope: verdicts on my r5 findings against the v1.5 deltas (:1388–1398 `run`-operand recording,
:1424–1453 shape-based rebind, :1308–1315 transit clause, :190/:869/:889/:906
scope-stamp→binding-identity, :724 env records), plus staging review of the deltas only.

## 1. Verdicts on my r5 findings

**r5-major (meta-6, `run`-target rebind channel) — the lookup dead-end is CLOSED; the writeback is
still wrong → narrowed MAJOR (finding 1).** The channel fix landed exactly as ruled: shape-split at
:1431–1443 routes qualified names through `symbol_named` + head/op discriminator, labels (`src:`,
`reify:`, `patch:`, user `#label`) through the **program-image label→head lookup** (the same
outbound `program()` walk the sideness test runs, verbatim `+d.name` match), with `code_named`'s
`d.code` walk as the stated fallback for `learn`-registered heads (:1442–1443); §6.7 codifies the
dual lookup space at :1583–1588 and `code_named`'s table row (:1005) now says "learned heads only".
Emission records the right key per operand class (:1408–1409). The dead-end — `code_named` scoped
to `d.code` while the only statically-known targets are program-side — is gone: the program-image
walk reaches literal heads through the `+d.values` emission-record anchor (:1284–1285), which sits
in the walk vocabulary (:1292). **But the rebind then writes the wrong payload**: the chain ends
"`→ head +d.entry → writeValue(slot)`" (:1440–1442), and the register row makes it uniform —
"then the head/op discriminator → `writeValue(slot, int64(entryOp))`" (:2030). `run`'s operand
slot holds `int64(head)`, not `int64(entry)`: §6.2 lowers `run` to "read the head's `+d.entry` for
the entry op" (:1076) and reads `result`/`produced` off the head's `+d.values` descriptor
(:1081–1084 — those cells "are reached by links, not ints", i.e. through the head at run time —
they cannot be folded: they are runtime-written cells of the *target*, reachable only via the
head's descriptor, and a static int64 to them would be a forbidden program-side ref-record per
:1426–1429). Writing the entry into a head-valued slot leaves `run` dereferencing `+d.entry`/
`+d.values` on an op cell at first post-resume execution — the learned fragment dies then instead
of at rebind-fault; same hazard, one hop later. Head-valued label slots are also what every other
`Code`-typed static operand needs (`let c := code"…"`, `graft_body(cl, code"…")`, `==` operands —
the undefined "fragment-identity records" of :1439 hint at this class and the shared chain
mistreats it identically: a `graft_body` operand rebound to the entry op is an M14-broken attach).
Only `Call`-operand slots take the entry payload. **Fix:** split the writeback by record kind —
call records (qualified-name arm) write `int64(entryOp)`; label-carrying fragment records write
`int64(resolved head)`, dropping the `+d.entry` hop — or state explicitly that `run`'s operand
slot is folded to a `#Call`-operand and name where the descriptor access gets its head.

**r5 finding 2 ("pre-promotion" inverts) — RESOLVED.** :1536–1537 now reads "handles carrying
their *promoted* targets (`arena_manifold.cpp:1947-1951` writes `real.at(target)`)" — verbatim the
fix.

**r5 finding 3 (stamp is a scope id, not a binding) — RESOLVED.** :190–192, :869–871, :889–894
all carry "the binding's identity — scope id *plus* binding ordinal within that scope, so two
bindings in one scope never share a stamp", and :195–196 / :908–911 say the `cap$N` is "stamped
with the captured binding's identity so the checker resolves it straight to that binding". The
stamp now literally names a binding; the diagnostics-channel separation (:724–726) is consistent.

**r5 finding 4 (sideness walk direction / live frames) — RESOLVED in substance, one mechanism
mis-attribution (finding 4).** Direction is now explicit on both sides: program-side is an
"outbound-following" `program()` walk (:1290); state-side is "link-reachable *from the agent
cell* — in either direction, as `promote` walks — **without transiting a program-side cell** (the
walk prunes at the program-side boundary)" (:1308–1311). The two-layer guard for frame-bound
captures landed: `learn` refuses them "by binding class ahead of the sideness test" (:1391–1392),
and the transit clause names "live frame cells … behind `home`'s `+d.cursors`/`+d.stack` ranks"
(:1312–1315). Residual: `home` is not actually a *program-side* anchor under the test — finding 4.

**r5 finding 5 micro-nits — 2 of 3 resolved, 1 carried.** `{cap$N → captured-name}` env record
(:724) now carries the name, matching its render-the-name purpose; genesis (e) names "the
shape-split rebind chain above" (:1531), no longer implying one channel. **Carried:** :738–739
still says "the `cap$N` alias and its capture-slot edge are established **per-activation** by
frame binding" — the binding-table alias is compile-time; per-activation is the edge's *target*.

## 2. New findings (v1.5 deltas only)

### 1. MAJOR — §6.6: the label-arm rebind writes `int64(entry)` into head-valued slots — `run` targets and Code operands die at first post-resume use

Detailed above (r5-major verdict): the program-image label→head lookup finds the right head, but
the shared "`→ head +d.entry → writeValue(slot)`" tail (:1440–1442, repeated in the P16 row at
:2030) stores the *entry op* where the slot's consumers need the *head* — `run` walks the head's
`+d.entry` **and** `+d.values` descriptor (arity/param cells, `result`/`produced` spill) at run
time (:1076–1084), and `graft_*`/stored `Code` operands need the head cell itself. As written, a
`learn`ed fragment containing `run code"…"` passes closedness, promotes, rebinds "successfully",
and fails on first execution — the r5 failure mode in a narrower costume. The same applies to the
`code_named` fallback arm for learned heads.

### 2. MINOR — §6.6: the `code_named`/`reflect`-born operand arm is unachievable as specified

:1408–1409 — "a `code_named`/`reflect`-born operand → its `d.name` label". Two problems. (a) A
`code_named("x")` operand lowers to a runtime call whose result lands in a runtime-written cell —
there is no emitter-minted int64 slot for the record's `+d.ref` to name; and the dynamic path
re-resolves post-genesis for free, so the record is dead weight unless the emitter *folds* the
operand — impossible, the learned head does not exist at emission. (b) A `reflect`-born operand
(`run(test_of(c))`) has no statically-known label at all — wrapper heads carry no stated `+d.name`
(the memo *record* holds the memo-kind label, :287), and `label_of` is `Opt`. Both belong to the
"opaque → closedness refusal" arm the same sentence ends with. **Fix:** drop the arm to
"`code_named`-labeled operands → the label argument; reflect results are opaque → refusal", or
define the deferred-slot emission that gives `+d.ref` a referent.

### 3. MINOR — §6.6/§6.7: the shape split routes `src:`/`reify:`/`patch:` labels through the qualified arm whenever a module is named `src`, `reify` or `patch`

The rebind "by shape" (:1431) resolves a qualified name through `module_named` on the first-`:`
prefix — and §6.7's rule treats `demo:` as a qualifier "only when it resolves as a module on
`programRef`'s walk" (:1575–1576, :1590–1592). A program with a module literally named `src`
(grammar-legal: `module src`, `ModulePath ::= Word { ':' Word }`) makes `src:demo:12:4` parse as
`symbol_named("src", "demo:12:4")` — symbol names never contain `:`, so the lookup misses → "an
unresolved name faults/drops the patch" (:1443–1444) → every learned fragment dies at genesis for
a module-naming choice. **Fix:** order the split — the reserved label namespaces (`src:`,
`reify:`, `patch:` prefixes; colon-less `#label`) match before the module-qualified arm — or
reserve the three prefixes.

### 4. MINOR — §6.6: `home` is not a program-side anchor under the stated test — the live-frame example mis-attributes its guard

:1312–1315 lists "`home`'s `+d.cursors`/`+d.stack` ranks" among the *program-side anchors* whose
transit the walk prunes at. But program-side is an *outbound-following* `program()` walk
(:1289–1290), and every edge at `home` runs `home`→rank (`+d.dims`, `+d.stdlib`, `+d.cursors` —
`vortex_core.cpp:46-114`, `vortex_vm.cpp:157-165`); `program()`→`home` outbound does not exist, so
`home` fails the sideness test's own reachability arm and is **not** program-side. The example is
saved only because `home` is link-unreachable from the agent cell (`##` anchors mint ref-boxes —
int content, invisible to the link walk) — unreachability, not transit-pruning, is the guard.
Worse, the door is open: a `cells"##/+d.items%"x"` creation linked into agent state makes `home`
agent-reachable bidirectionally → `home`, its cursor rank and every live frame classify
state-side → promote → live frames in the checkpoint, exactly what the sentence claims cannot
happen. **Fix:** name the real guard — "`home` carries no link the agent can reach; live frames
are unreachable, and frame-bound captures are refused by binding class before sideness" — or add
`home` to the program-side set by fiat (adapter marks it, like the recorded extent).

### 5. MINOR — M13 interaction: template-expanded literals share `src:` labels → the label→head lookup is ambiguous across expansion sites

A `code"…"` literal inside a `Code`-splicing `def` is `cloneExpr`'d per call site (M13, :405–421)
— each clone's head carries `+d.name →` the *same* `src:<def-position>` label (the
`SourceLocation` keying at :903–905 shares the original's record). N sites mint N heads under one
label → the program-image label→head lookup (:1440–1442) is first-match-ambiguous, and a
`run`-target record inside an expanded site can rebind *another* site's clone head — wrong
captures, silently. The `+o:c` offset chain (:920–921) disambiguates *nested* literals only, not
template-site copies. **Fix:** extend the label for expanded copies (`src:m:l:c+o:c` per
expansion site, reusing the nested-offset machinery) or refuse `run`-target records on
template-cloned literals.

### 6. Micro-nits

- :1439 — "fragment-identity records" is named once and never defined; if it is the class for
  `let`-bound `Code` / `graft`-operand slots, finding 1 is its blocking question.
- :962 — the head-layout legend still says "`+d.name`→qualified name ⇒ NAME-record"; it should
  name the label arm too.
- :137 (glossary `closed`) — "a captured field/module-var binding": no module-level `VarDecl`
  exists (`VarDecl` lives only inside `skill`/`behavior`/`agent` members, spec :1326–1332); the
  class means declared field cells — the phrase invites exactly the program-side-capture
  misreading.
- :738–739 — carried r5 nit: the `cap$N` *alias* is compile-time; per-activation is the slot
  edge's target.

## 3. Staging discipline summary

The pipeline contract that was open last round — which rank each durable name lives on — is now
answered and answered correctly: `d.vars`/`symbol_named` for member calls, the program-image
structural walk for literal-born labels, `d.code`/`code_named` for learned heads (:1583–1588
states the split plainly). What remains un-priced at the same joint is the *payload* each record
writes: `Call` slots take `int64(entryOp)`; `Code`-handle slots take `int64(head)` — one bit of
record metadata or one clause each at :1438–1442 and :2030 closes it. The transit clause and
binding-identity stamps are clean. The residual hazard surface is once again the newest sentence,
not the machinery.

VERDICT: NOT CONVERGED — 1 major (label-arm writeback is entry where head-valued slots need the
head), 4 minors, 4 nits.
