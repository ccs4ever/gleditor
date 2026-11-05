# r2-meta — metaprogramming / staging-semantics freeze pass

**Spec:** `design/vagent-actor-executor.md` v0.2 (post-r1 revision)
**Lane:** does the executor model respect the established staging/durability mechanics —
code-as-cells, fragments, graft/learn EDL, genesis replay, P15/P16.
**Counts:** BLOCKING 0 · MAJOR 1 · MINOR 5 · NIT 7 (13 findings)

______________________________________________________________________

## Disposition verification (r1-meta findings → v0.2)

| r1 # | Disposition                                                                                                                                   |
| ---- | --------------------------------------------------------------------------------------------------------------------------------------------- |
| 1 (B) clause weights are keyed `d.vars` records, not `+d.weight` junction edges        | **Landed.** §2.1 (:80-85) now states the record channel verbatim and cites both arms (`meta.cpp:602-619,:2038-2064`); §3.4 (:232-243) prices the real O(V+C) snapshot, absent→0.0, rank-position tie-break. Residual spec-depth items in finding 6 below. |
| 2 (B) `reify:` labels unstable across genesis replay                                    | **Landed.** §3.5 (:255-268) records the *registered-head name*. **Stability verified**: genesis (d) re-mints the clause's `+d.name` verbatim from the patch record's label (`arena.makeCell(label)`, `genesis.cpp:286-294`) — a replayed `patch:<n>` **keeps n**, never redraws; the recorded `+d.patch-head` names (`records.hpp:103-114`, minted `meta.cpp:1673-1688`) are durable `d.patch`-rank data re-resolved via `codeNamed` → `reify` (`genesis.cpp:247-262`); registered heads ride the agent cell's `+d.code` rank (state-side, folds verbatim); P16 re-kinds only Int64 cells naming *ephemeral* referents (`p16.cpp:104-108`), so `+d.name` text and `labelOf` survive checkpoint→resume→rehydration. Residual spec-depth gap in finding 2. |
| 3 (M) "no-marks satisfied by construction" wrong under m0                                | **Landed.** §3.8 (:308-313) now gives the trailed-writes basis, names `resolveDimension` as the real gate (`meta.cpp:818-825`), and limits `mintMetaBindSet`'s rule to its callers. Residual: the `d.clone` example is overstated (finding 3). |
| 4 (M) `d.agents` mint stage + positional invariants unpinned                              | **Partially superseded.** The directory-cell redesign (B5) removes `home`'s positional worry (one Additive `+d.dirs` add, no clears ever) and narrows `dirCell`'s expansion to `+d.agents` adds only — nothing else in the spec writes onto it (tombstone sits on the agent cell, :152). The surviving half — `agentCell`'s placement vs `imageExtent` — is still unpinned (finding 4). |
| 5 (M) segmented-body emission contract conventions unnamed                                 | **Not landed.** §3.5 still says only "entry dispatches on the persisted `step` path into the segment spine"; the read channel, per-segment Return tails, and the no-new-descriptor-fields guard are all absent (finding 1 — MAJOR, carried). |
| 6 (m) enumeration freshness                                                              | **Landed substantively.** "Walks `d.clause` fresh — never a cached snapshot" (:233) is the right claim; explicit same-turn wording, the `fault:`/`unlearn:` asymmetry note, and `set_weight` in the enumeration are still absent (NITs). |
| 7 (m) `learn_where` asymmetry deliberate                                                  | **Landed.** "Repoints `+d.grab` undetected — intended per §9.2" (:265-266). |
| 8 (m) replay-vs-live discipline unstated                                                  | **Partial.** The trailed-writes atomicity basis landed (§3.8); "re-enacts without re-recording; the executor must neither suppress nor special-case replay" is still unstated (NIT). |
| 9 (n) miscites                                                                            | **Half.** §2.1 now cites `weightOf` at :602-619; §3.5 still cites `meta.cpp:1850-1851` for the `graft_body`+`learn_body` repoint (learnBody only — graftBody is :1338-1347). |

Round-2 scope checks: corrected weight path ✓; dirCell promote story ✓ (expansion stays
`+d.agents`-only by construction — no other writer exists in the spec); named-identity
rehydration ✓ (mechanics above); full §3.2 contract vs agent-language §4.2/turn-end ✓
(base CP, Dewey reset, per-turn scope unlink, `compact_every`, `clearChoicePoints`→`release`,
non-intention failure arm all present :169-200); milestone rebasing ✓ (segmentation+ABI in M2
before M3 consumes it; M3 records the name comparand for M4).

______________________________________________________________________

## Findings

### 1. MAJOR — §3.5/§8: the resume-side emission contract is still unspecified (carried r1-f5)

**Spec location:** `vagent-actor-executor.md:250-253` (§3.5), `:292-321` (§3.8), `:469-473` (M2).

**Problem.** v0.2 lands the *verb-call* half of the seam (§3.8's `+d.verb` ABI) but not the
*resume* half. "Entry dispatches on the persisted `step` path into the segment spine" still
does not say through what channel the dispatched preamble reads the path, and the two
structural conventions the read depends on are unnamed — the same gap r1 finding 5 priced,
never dispositioned into text.

**Evidence.**

- The step path cannot be a descriptor parameter — `signatureOf`/`clauseFormals` compare
  parameter lists (`meta.cpp:1143-1165`, `:1366-1389`), so a synthetic `step` param fails
  every graft/learn signature check — and it cannot be an extra `runFragment` argument
  (the arity check refuses arg-count ≠ param-count, `meta.cpp:710-713`). What remains is a
  **reserved non-parameter binding** on the cursor's `+d.vars` (the `$`-convention,
  `vagent-agent-language.md:619-623`) written by the executor before re-entry, or an
  executor-side register the preamble opcode reads. v0.2's "`$`-reserved seeds" (:453)
  gestures at the family but never names `$step` or its owner.
- The path's *shape* is hierarchical — "the list of statement ordinals down to it"
  (`vagent-agent-language.md:904`), with `while`/`for` re-entry re-testing from frame
  variables (`:905-906`) — flat index vs ordinal-list walk emit different dispatch
  preambles; the spec still doesn't say which.
- Per-segment `Return` tails: `runFragment`/`#Call` convention and `validate`'s
  run-reachable rule (`meta.cpp:561-568`) expect Return-tailed graphs; a suspending segment
  ends in an outbox-stage, not a Return — the emission contract must state what each
  segment's tail is.
- The descriptor guard ("no new descriptor fields" — M2 must not mint a `step` parameter
  record or `signatureOf` comparisons break) is unstated.

**Fix.** One emission-contract paragraph in §3.5 or §3.8: `+d.entry` → dispatch preamble
reading a named reserved binding (`$step`, ordinal-list encoding, bound by the executor at
re-entry); per-segment tail stated; "no new descriptor fields" said explicitly. M2 already
owns segmentation — this is its acceptance text.

### 2. MINOR — §3.5: the registered-name *derivation* is unpinned; two natural readings each fail

**Spec location:** `vagent-actor-executor.md:255-264`.

**Problem.** The stability claim is verified (disposition table above) — but "the
registered-head name of its clause's `+d.spin` target" never says how to *obtain* that name
from an install copy, and both naive readings silently reintroduce the r1 bug in one
regime.

**Evidence.**

- `labelOf(spinTarget)` reads the copy's own `+d.name` = `reify:<src>:<serial>`
  (`meta.cpp:1101-1105`) — unstable across replay (the spec itself warns against this).
- `labelOf(+d.entry target)` works only post-replay: replayed installs delegate to the
  registered head (`genesis.cpp:257-261`), while live-session installs delegate to the
  *original* fragment head (`learnCopies` reifies the argument head for both copies,
  `meta.cpp:1584-1588`; `+d.entry` delegation `:882`), whose label is a `src:`/`#` fragment
  label, not the registered name.
- The uniform channel is the one genesis itself uses — the `+d.patch-head` chain:
  `priorAttachName(clauseLabel, dims.spin)` (`meta.cpp:1618-1644`) returns the newest
  spin-touching patch's recorded registered name; fall back to `labelOf(target)` when no
  patch covers the attach (declared bodies, session grafts — whose cells persist as
  state-side diffs, so their own labels are stable). The spec never names it.
- "`patch:<n>`/`codeNamed` names for installed copies" conflates three kinds: `patch:<n>`
  is the *clause/patch* label (`learnPlan` mints it on clause + record,
  `meta.cpp:1726-1734`) — never a head name; `codeNamed` is a resolver (`meta.cpp:251-284`),
  not a name kind; the names it resolves for learned installs are the registered heads'
  `reify:` labels.
- Unstated corners: the declared body-head name's *formation rule* (the channel exists —
  `+d.name` + `codeNamed`'s image-walk fallback `meta.cpp:259-283`, and `compileFragment`
  already writes `+d.name` for `code"…"` literals, `fragment.cpp:180-186` — but
  decl-lowered bodies need a stated spelling and uniqueness under first-match); nameless
  `+d.spin` targets (`graftable` checks shape, not name — a session graft can install an
  unnamed head); and whether the compare gates the *first* dispatch too (repoint between
  adoption and first segment, not only resume-after-suspension).

**Fix.** One sentence naming the derivation — `priorAttachName(clause, spin)` when a
spin-touching patch exists else `labelOf(+d.spin target)` — plus: drop `patch:<n>` from the
head-name list (it is the clause label); state the declared-name formation rule (e.g.
`module:Agent.plan` qualified, unique under `codeNamed` first-match); state the
nameless-target rule (record labelOf-or-refuse, or fault on absent name); and run the
compare at every body entry including the first.

### 3. MINOR — §3.1/§3.8: the `d.clone` example is wrong; "recipe dims in `kAgentDimensionNames`" is the wrong container

**Spec location:** `vagent-actor-executor.md:116-117` (§3.1), `:311-313` (§3.8), `:454-455` (M0).

**Problem.** "`d.clone` … a mid-turn `learn_*`→`reify` would refuse" — it cannot: `d.clone`
is a **genesis dim**, minted at `initGenesis` (`vortex_core.cpp:57`) and already filed via
`kGenesisDimensionNames` (`lineage.hpp:48`), so `resolveDimension`'s first arm always hits
(`meta.cpp:818-819`). The refusal hazard is real but lives elsewhere: unfiled *recipe-member*
dim names — which are data-dependent and cannot be a constant-table entry.

**Evidence.** Recipe-referenced names file through two existing channels, neither of which
is `kAgentDimensionNames`: the emission-time `fileDimensionNames` list covering source-named
`dimConstant` mints (`lineage.hpp:168-176`, `compiler.cpp:769-772`) for image-declared
heads, and genesis (b)'s `sweepDimensionVocabulary` walking `+d.code` members' `+d.recipe`
`+d.name` fields (`genesis.cpp:169-173`) for registered heads. The four genuinely new names
(`d.agents`/`d.dirs`/`d.seen`/`d.evidence`) are correctly listed for the constant (absent
today, `lineage.hpp:67-71`). Also unnamed: `+d.verb` — the §3.8 ABI dim the M2 emitter
mints — will file via the emitter's `arenaDims_` sweep but belongs in M0/M2's filing
inventory by name.

**Fix.** Correct the example: `d.clone` is pre-filed (say so); the mid-turn refusal class is
*unfiled recipe `dimConstant` names* — filed by the emission list plus the (b) `+d.code`
sweep, not `kAgentDimensionNames`. Add `d.verb` to the named filing inventory.

### 4. MINOR — §3.1: `agentCell > imageExtent` remains an unstated precondition of its own Clears claims

**Spec location:** `vagent-actor-executor.md:143-153` (§3.1, "(state-side `Clears`)"), `:453-456` (M0).

**Problem.** §3.1 invokes "state-side `Clears`" for agent-cell tombstones and relies on
edge-removal semantics across its new ranks — but `commitStateSide` returns `Clears` only
for `ref > extent` (`arena_manifold.hpp:278-281`; the same test seeds `extraRoots`,
`lineage.cpp:190-201`), and nothing pins the agent cell above the image band. The dirCell
redesign narrowed this considerably versus r1-f4, but a residue survives.

**Evidence.** Repoints and adds commit under Additive, so most unlink paths are safe;
`tombstone` filtering covers the EDL ranks (`d.patch`/`d.code` resurrections are filtered by
`unlearn:`/`fault:` regardless, `meta.cpp:1922-1996`, `clausesOf` `:210-220`). The residue is
**singleton-rank clears sourced on the agent cell**: `unlinkRank`'s tail arm
(`records.hpp:46-52`) removes the predecessor's posward edge — when the sole member of an
agent-owned rank is removed and the predecessor is the agent cell itself, the clear emits on
the agent cell and commits only if it is state-side. Cases: last `+d.code` de-registration,
last `d.patch` drop (tombstone-covered), and — not tombstone-covered — `d.seen` ring
eviction to empty, the last `d.outbox`/`d.pending`/`d.belief` removal (resurrected entries
re-deliver or silently un-forget post-genesis; outbox duplicates are at least
idempotence-detectable, :289-290). The mint site is also still unstated: `runGenesis` takes
`agentCell` as an *input* root (`genesis.hpp:79-84`), so M0 chooses where it first promotes
— inside the sealed image (≤ extent, breaks the above) or after (state-side, works).

**Fix.** One M0 acceptance line: the agent cell (and with it the directory cell) is minted
post-image so its ref lands `> imageExtent`; add the genesis-level assert. Tombstones make
correctness survivable either way — the pin is what makes `d.seen`'s bounded-by-construction
and the Clears citations honest.

### 5. MINOR — §3.8: the clause-walk cite names the wrong document; the fragment's belief anchor is unstated

**Spec location:** `vagent-actor-executor.md:298-302`.

**Problem.** "`vagent-metaprogramming.md` §9.1's clause-walk lowering" — that document's
§9.1 is the `splice` worked example (`vagent-metaprogramming.md:2790-2806`). The prescribed
lowering lives in **`vagent-agent-language.md` §9.1's cross-walk** (:1500): "`believes t` |
clause walk with `#Unify`, one `#Choice` per clause | none" — which is exactly the
pure-arena subcircuit §3.8 describes, cited to the wrong sibling.

**Evidence.** Additionally unstated: inside a `code"…"` fragment there is *no host call*, so
the emitted clause walk needs a stated anchor to the agent's `d.belief` ranks — the natural
channel is a `$`-reserved `self`/agent-cell binding on the cursor's `+d.vars` (the same
binding channel `runFragment` uses, `meta.cpp:741`), which M0's "`$`-reserved seeds" should
name explicitly rather than leave to discovery.

**Fix.** Re-cite to `vagent-agent-language.md` §9.1 (:1500); name the fragment's
belief-store anchor (reserved `self` binding bound per dispatch).

### 6. MINOR — §3.4: the weight snapshot's rank is shared, and `set_weight` is missing from the freshness enumeration

**Spec location:** `vagent-actor-executor.md:232-243`.

**Problem.** "One walk of the agent's `d.vars` weight-record rank" reads as a dedicated
rank; it is the **shared** `d.vars` rank — `meta.serial` (`meta.cpp:339-364`),
`unlearn:<label>` (`:323-330`), `fault:<label>` (`:1917-1921`), and M0's field records all
ride it — and `weightOf`'s read is kind-filtered (`asDouble` on `+d.values`, `:614-616`),
which the snapshot must mirror or state its filter. `set_weight` is also absent from the
"take effect at the next selection" enumeration (:233-234) even though §9.3's `Meal`
handler is precisely a mid-turn `set_weight` → next-selection dependency; and the
fresh snapshot-per-selection reading is implied but never said ("possibly within the same
turn" was the r1 wording request). V's monotone growth (tombstone/serial/weight records are
never reclaimed) goes unpriced in the O(V+C) claim.

**Fix.** Three clauses: the snapshot walks the shared `d.vars` rank and maps records whose
`+d.values` reads `asDouble` (or states its key rule); add `set_weight` to the
freshness enumeration and say the rebuild is per-selection, so mid-turn writes land at the
next selection inside one turn; note V grows with patch history.

### 7. NIT — carried-over miscite and wording slips

- §3.5 `:263`: `meta.cpp:1850-1851` cited for "`graft_body`/`learn_body` install a fresh
  named head" — that range is `learnBody`'s repoint only; `graftBody` is
  `meta.cpp:1338-1347`, and for an unregistered argument `installable` links the *existing*
  head (`meta.cpp:1222-1228`), not "a fresh" one.
- §3.2 `:175`: "(coalesced, §4.1)" — the coalescing semantics live in §3.3, not §4.1.
- §3.8 `:319-320`: "refused/`expected`-error verb → … `fail_step`" overstates — a statement
  failure in a handler takes the statement backtrack arm; `fail_step` is the plan/intention
  path.
- §3.1 `:150`: "carries only `+d.dirs`-reached membership ranks" — the cell carries
  `+d.agents`; it is *reached* via `+d.dirs`.
- Still absent (r1-6): the `fault:`-vs-`unlearn:` tombstone asymmetry note (genesis
  `dropPatch` mints `fault:` only, `genesis.cpp:219-228`; `undoPatch`'s `dropDead` mints
  both, `meta.cpp:1917-1927`).
- Still absent (r1-8): the "re-enacts without re-recording; no replay-mode special-casing"
  paragraph.
- The agent-delisting tombstone's key (`unlearn:<what>`) is unnamed — `cells"…"` consumers
  need the spelling to filter.

______________________________________________________________________

## Lane summary

v0.2 lands every blocking disposition from my lane correctly: the weight channel is now the
truth (`d.vars` label-keyed records, honest O(V+C) pricing), the bound-body identity moved
to the registered-head name — which I verified end-to-end survives genesis (d) verbatim
(`patch:<n>` re-mint, `patch-head`→`codeNamed`→`reify`, `+d.code` state-side durability,
P16's ephemeral-only re-kind) — the no-marks basis is corrected to trailed-writes, the
`learn_where` asymmetry is declared deliberate, and §3.2 now carries the full §4.2 turn
contract. What remains is specification depth, not direction: the resume-side emission
contract (`$step` channel, segment tails, descriptor guard) is the one carried MAJOR —
it names no channel and has a proven-wrong implementation (a `step` parameter fails every
signature check). Everything else is cite hygiene, filing-container precision, and one
residual positional pin for the agent cell.

VERDICT: NOT CONVERGED
